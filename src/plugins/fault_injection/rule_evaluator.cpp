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

#include <algorithm>

namespace nixl::fault {

ruleEvaluator::ruleEvaluator(const faultConfig &config) : activeRules_(config.getRules().size()) {
    rules_.reserve(config.getRules().size());
    for (const auto &rule : config.getRules()) {
        rules_.push_back({.rule = rule, .remaining = rule.injectOnOccurrence});
    }
}

const faultRule *
ruleEvaluator::evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation) {
    if (activeRules_.load(std::memory_order_relaxed) == 0) {
        return nullptr;
    }
    const auto matches = [method, operation](const ruleState &state) {
        return state.rule.method == method &&
            (!state.rule.operation || state.rule.operation == operation);
    };
    // Only immutable rule definitions are read before locking, never the countdowns.
    if (std::none_of(rules_.begin(), rules_.end(), matches)) {
        return nullptr;
    }
    const std::lock_guard lock(mutex_);
    const faultRule *firing = nullptr;
    for (auto &state : rules_) {
        if (!matches(state) || state.remaining == 0) {
            continue;
        }
        // Count down to zero so spent rules cannot wrap around and fire again.
        // The mutex lets only one caller make the transition from 1 to 0.
        if (--state.remaining == 0) {
            firing = &state.rule;
            activeRules_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    return firing;
}

} // namespace nixl::fault
