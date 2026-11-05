/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rule_evaluator.h"
#include "common/nixl_log.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iomanip>

namespace nixl::fault {
namespace {

    // Decrement unless the counter already reached zero, and report the value it held
    // beforehand. Only the caller that sees 1 took the counter to zero, so only that caller
    // fires the rule.
    uint64_t
    claimOccurrence(std::atomic<uint64_t> &remaining) {
        uint64_t current = remaining.load(std::memory_order_relaxed);
        while (current != 0) {
            const bool claimed =
                remaining.compare_exchange_weak(current, current - 1, std::memory_order_relaxed);
            if (claimed) {
                break;
            }
            // The failed exchange refreshed current with the live counter; try again.
        }
        return current;
    }

    // Does this rule select the call being evaluated? Asked twice below, once to decide
    // whether ordering is still needed and once to claim the occurrence, so a selector added
    // to one spelling and not the other would quietly change which calls fire.
    bool
    matchesCall(const faultRule &rule,
                backend_method_t method,
                std::optional<nixl_xfer_op_t> operation) {
        return rule.method == method &&
            (!rule.operation.has_value() || rule.operation == operation);
    }

} // namespace

ruleEvaluator::ruleEvaluator(const faultConfig &config) {
    rules_.reserve(config.getRules().size());
    for (const auto &rule : config.getRules()) {
        // Different firing occurrences can still count the same backend calls.
        const bool shares_matching_calls =
            std::any_of(rules_.begin(), rules_.end(), [&rule](const auto &existing_state) {
                return existing_state.rule.method == rule.method &&
                    (!existing_state.rule.operation || !rule.operation ||
                     existing_state.rule.operation == rule.operation);
            });
        // Record each affected method once; its counters must agree on concurrent call order.
        if (shares_matching_calls &&
            std::find(serializedMethods_.begin(), serializedMethods_.end(), rule.method) ==
                serializedMethods_.end()) {
            serializedMethods_.push_back(rule.method);
        }
        rules_.emplace_back(rule, rule.injectOnOccurrence);
    }
}

const faultRule *
ruleEvaluator::evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation) {
    // A method and operation that disagree match no directional rule, so those rules never
    // fire and the run still looks clean. The parser holds rule definitions to the same
    // pairing, so only a forwarder call site can break it.
    NIXL_ASSERT_ALWAYS(operation.has_value() == carriesOperation(method))
        << "method=" << methodName(method) << " - the call site and carriesOperation() disagree";
    // One call decrements every counter that matches it, and grouping those decrements is what
    // this lock is for. Each decrement is atomic on its own, but two concurrent calls can
    // interleave so that one counter ranks them A then B while another ranks them B then A.
    // The call that comes first on both then claims two rules, and only one status can be
    // returned, so the other rule's counter reaches zero without its status ever reaching a
    // caller. These rules are one-shot, so it is gone: the run quietly injects one fault fewer
    // than the configuration asked for, with nothing to show it happened.
    std::unique_lock lock(orderingMutex_, std::defer_lock);
    if (std::find(serializedMethods_.begin(), serializedMethods_.end(), method) !=
        serializedMethods_.end()) {
        const bool active =
            std::any_of(rules_.begin(), rules_.end(), [method, operation](const auto &rule_state) {
                return matchesCall(rule_state.rule, method, operation) &&
                    rule_state.remaining.load(std::memory_order_relaxed) != 0;
            });
        if (!active) {
            return nullptr;
        }
        lock.lock();
    }
    const faultRule *firing = nullptr;
    for (auto &rule_state : rules_) {
        // Rule definitions are immutable, so a call that matches nothing touches no
        // shared state at all.
        if (!matchesCall(rule_state.rule, method, operation)) {
            continue;
        }
        if (claimOccurrence(rule_state.remaining) == 1) {
            // The parser rejects rules that can fire on the same call, so at most one
            // reaches zero here. A future selector that overlapReason does not know about
            // would break that, and the rule left behind is already spent, so it never
            // fires at all. Say so before taking the later one, because release builds
            // define NDEBUG and lose the assert that would otherwise stop here.
            if (firing != nullptr) {
                NIXL_WARN << "event=nixl_fault_double_claim dropped_rule_id="
                          << std::quoted(firing->id)
                          << " kept_rule_id=" << std::quoted(rule_state.rule.id)
                          << " method=" << methodName(method)
                          << " - one call claimed two rules and only one status is returned";
            }
            assert(firing == nullptr);
            firing = &rule_state.rule;
        }
    }
    return firing;
}

} // namespace nixl::fault
