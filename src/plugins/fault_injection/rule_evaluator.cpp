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

namespace nixl::fault {

ruleEvaluator::ruleEvaluator(const faultConfig &config) {
    rules_.reserve(config.getRules().size());
    for (const auto &rule : config.getRules()) {
        rules_.emplace_back(rule, rule.injectOnOccurrence);
    }
}

const faultRule *
ruleEvaluator::evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation) {
    const faultRule *firing = nullptr;
    for (auto &rule_state : rules_) {
        // Rule definitions are immutable, so a call that matches nothing touches no
        // shared state at all.
        if (rule_state.rule.method != method ||
            (rule_state.rule.operation && rule_state.rule.operation != operation)) {
            continue;
        }
        // Stop at zero so a spent rule cannot wrap around and fire again. Exactly one
        // caller can win the decrement from 1, and that caller fires the rule. Relaxed
        // suffices: the counter publishes no state, and the rule it guards was written
        // before any caller could reach this evaluator.
        uint64_t remaining = rule_state.remaining.load(std::memory_order_relaxed);
        while (remaining != 0 &&
               !rule_state.remaining.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {}
        if (remaining == 1) {
            firing = &rule_state.rule;
        }
    }
    return firing;
}

} // namespace nixl::fault
