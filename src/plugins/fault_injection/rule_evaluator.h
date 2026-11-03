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
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace nixl::fault {

/** Owns validated rules and independent occurrence counters.
 *
 * Construct exactly one per loaded proxy plugin, beside the genuine plugin it wraps, and
 * hand backend engines a reference to it. The one-shot guarantee is a property of this
 * object: a second evaluator over the same configuration restarts every count, so an
 * evaluator per engine would inject each rule once per engine instead of once per run.
 *
 * Concurrent calls share those counters; each rule fires at most once.
 */
class ruleEvaluator {
public:
    explicit ruleEvaluator(const faultConfig &config);

    // Engines hold a reference to this evaluator, and moving it would relocate the rules
    // they point at, so it stays put once constructed.
    ruleEvaluator(const ruleEvaluator &) = delete;
    ruleEvaluator &
    operator=(const ruleEvaluator &) = delete;
    ruleEvaluator(ruleEvaluator &&) = delete;
    ruleEvaluator &
    operator=(ruleEvaluator &&) = delete;

    /** Count one backend call and return its firing rule, or nullptr to forward.
     * Pass the operation the call carried, or std::nullopt for the methods that take none;
     * carriesOperation(method) says which, and the pairing is not optional because a
     * missing operation silently matches no rule.
     * Every matching rule advances, including when another rule fires on this call, so an
     * occurrence counts calls made rather than calls the genuine backend ran.
     * The returned rule is immutable and remains valid until this evaluator is destroyed.
     */
    [[nodiscard]] const faultRule *
    evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation);

private:
    struct ruleState {
        ruleState(faultRule definition, uint64_t count)
            : rule(std::move(definition)),
              remaining(count) {}

        // An atomic member leaves ruleState uncopyable, which std::vector needs for
        // growth. Construction is single threaded, so moving the counter's value is safe.
        ruleState(ruleState &&other) noexcept
            : rule(std::move(other.rule)),
              remaining(other.remaining.load(std::memory_order_relaxed)) {}

        faultRule rule;
        // Counts down to zero once; the thread that claims the last decrement fires.
        std::atomic<uint64_t> remaining;
    };

    std::vector<ruleState> rules_;
    // Overlapping selectors must advance together to preserve one call order across rules.
    std::vector<backend_method_t> serializedMethods_;
    std::mutex orderingMutex_;
};

} // namespace nixl::fault

#endif
