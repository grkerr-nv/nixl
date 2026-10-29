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

#ifndef NIXL_SRC_PLUGINS_FAULT_INJECTION_FAULT_CONFIG_H
#define NIXL_SRC_PLUGINS_FAULT_INJECTION_FAULT_CONFIG_H

#include "nixl_types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nixl::fault {

/** Status-returning backend methods supported by schema version 1. */
enum class backend_method_t {
    REGISTER_MEM,
    DEREGISTER_MEM,
    CONNECT,
    DISCONNECT,
    UNLOAD_MD,
    PREP_XFER,
    POST_XFER,
    CHECK_XFER,
    RELEASE_REQ_H,
    PREP_MEM_VIEW,
    GET_PUBLIC_DATA,
    GET_CONN_INFO,
    LOAD_REMOTE_CONN_INFO,
    LOAD_REMOTE_MD,
    LOAD_LOCAL_MD,
    GET_NOTIFS,
    GEN_NOTIF,
    QUERY_MEM,
    ESTIMATE_XFER_COST,
};

/** A validated rule definition. Occurrence counters belong to the runtime evaluator. */
struct faultRule {
    std::string id;
    backend_method_t method;
    std::optional<nixl_xfer_op_t> operation;
    uint64_t injectOnOccurrence;
    nixl_status_t status;
};

/** Owns a validated configuration snapshot without loading plugins or evaluating rules. */
class faultConfig {
public:
    /** Parse schema version 1. Throws std::invalid_argument for invalid TOML or schema. */
    [[nodiscard]] static faultConfig
    parse(std::string_view text);

    /** Read and parse one file. Throws std::runtime_error for file access failures. */
    [[nodiscard]] static faultConfig
    load(const std::filesystem::path &path);

    [[nodiscard]] const std::string &
    getExperimentId() const {
        return experimentId_;
    }

    [[nodiscard]] const std::filesystem::path &
    getRealPluginPath() const {
        return realPluginPath_;
    }

    [[nodiscard]] const std::vector<faultRule> &
    getRules() const {
        return rules_;
    }

private:
    faultConfig() = default;

    std::string experimentId_;
    std::filesystem::path realPluginPath_;
    std::vector<faultRule> rules_;
};

} // namespace nixl::fault

#endif
