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

#include <atomic>
#include <cassert>
#include <cstdint>

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

} // namespace

ruleEvaluator::ruleEvaluator(const faultConfig &config) {
    rules_.reserve(config.getRules().size());
    for (const auto &rule : config.getRules()) {
        rules_.emplace_back(rule, rule.injectOnOccurrence);
    }
}

const faultRule *
ruleEvaluator::evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation) {
    // A method and operation that disagree match no rule, so the call injects nothing and
    // the run looks clean. The parser holds rule definitions to the same pairing.
    assert(operation.has_value() == carriesOperation(method));
    const faultRule *firing = nullptr;
    for (auto &rule_state : rules_) {
        // Rule definitions are immutable, so a call that matches nothing touches no
        // shared state at all.
        if (rule_state.rule.method != method ||
            (rule_state.rule.operation && rule_state.rule.operation != operation)) {
            continue;
        }
        if (claimOccurrence(rule_state.remaining) == 1) {
            // The parser rejects rules that can fire on the same call, so at most one
            // reaches zero here. Assert it rather than quietly discarding the other: a
            // future selector that overlapReason does not know about would otherwise
            // inject whichever status came later in the file.
            assert(firing == nullptr);
            firing = &rule_state.rule;
        }
    }
    return firing;
}

} // namespace nixl::fault
