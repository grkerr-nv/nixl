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

#include "fault_runtime.h"
#include "common/nixl_log.h"

#include <iomanip>
#include <utility>

namespace nixl::fault {

faultRuntime::faultRuntime(faultConfig config) : config_(std::move(config)), evaluator_(config_) {}

void
faultRuntime::logRule(const char *event,
                      const faultRule &rule,
                      std::optional<nixl_xfer_op_t> operation) const {
    NIXL_INFO << "event=" << event << " experiment_id=" << std::quoted(config_.getExperimentId())
              << " config_digest=" << config_.getDigest() << " rule_id=" << std::quoted(rule.id)
              << " method=" << methodName(rule.method) << " operation="
              << (operation ? operationName(*operation) :
                              (carriesOperation(rule.method) ? "any" : "none"))
              << " occurrence=" << rule.injectOnOccurrence << " status=" << statusName(rule.status);
}

void
faultRuntime::logConfiguration() const {
    NIXL_INFO << "event=nixl_fault_config"
              << " experiment_id=" << std::quoted(config_.getExperimentId())
              << " config_digest=" << config_.getDigest()
              << " real_plugin_path=" << std::quoted(config_.getRealPluginPath().string())
              << " rule_count=" << config_.getRules().size();
    for (const auto &rule : config_.getRules()) {
        logRule("nixl_fault_rule", rule, rule.operation);
    }
}

std::optional<nixl_status_t>
faultRuntime::evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation) {
    const auto *rule = evaluator_.evaluate(method, operation);
    if (!rule) {
        return std::nullopt;
    }
    logRule("nixl_fault_injection", *rule, operation);
    return rule->status;
}

} // namespace nixl::fault
