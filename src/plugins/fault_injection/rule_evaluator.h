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

#ifndef NIXL_SRC_PLUGINS_FAULT_INJECTION_RULE_EVALUATOR_H
#define NIXL_SRC_PLUGINS_FAULT_INJECTION_RULE_EVALUATOR_H

#include "fault_config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace nixl::fault {

/** Owns validated rules and independent occurrence counters for one runtime.
 * Concurrent calls share those counters; each rule fires at most once.
 */
class ruleEvaluator {
public:
    explicit ruleEvaluator(const faultConfig &config);

    /** Count one backend call and return its firing rule, or nullptr to forward.
     * Every matching rule advances, including when another rule fires on this call.
     * The returned rule is immutable and remains valid until this evaluator is destroyed.
     */
    [[nodiscard]] const faultRule *
    evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation = std::nullopt);

private:
    struct ruleState {
        faultRule rule;
        uint64_t remaining;
    };

    std::vector<ruleState> rules_;
    // Monotonic count for the unlocked spent-rules bypass; it publishes no other state.
    std::atomic<std::size_t> activeRules_;
    // Protects occurrence updates and one-shot selection; rule definitions never change.
    std::mutex mutex_;
};

} // namespace nixl::fault

#endif
