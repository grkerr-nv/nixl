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

#ifndef NIXL_SRC_PLUGINS_FAULT_INJECTION_FAULT_RUNTIME_H
#define NIXL_SRC_PLUGINS_FAULT_INJECTION_FAULT_RUNTIME_H

#include "rule_evaluator.h"

namespace nixl::fault {

/** One configuration and evaluator per loaded proxy, retained by every engine. */
class faultRuntime {
public:
    explicit faultRuntime(faultConfig config);

    [[nodiscard]] const faultConfig &
    getConfig() const {
        return config_;
    }

    /** Select and log one injected result, or return nullopt to forward normally. */
    [[nodiscard]] std::optional<nixl_status_t>
    evaluate(backend_method_t method, std::optional<nixl_xfer_op_t> operation);

    void
    logConfiguration() const;

private:
    void
    logRule(const char *event,
            const faultRule &rule,
            std::optional<nixl_xfer_op_t> operation) const;

    const faultConfig config_;
    ruleEvaluator evaluator_;
};

} // namespace nixl::fault

#endif
