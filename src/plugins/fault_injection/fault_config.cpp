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

#include "fault_config.h"

#include "table_scope.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nixl::fault {
namespace {

    constexpr std::pair<std::string_view, backend_method_t> methods[] = {
        {"registerMem", backend_method_t::REGISTER_MEM},
        {"deregisterMem", backend_method_t::DEREGISTER_MEM},
        {"connect", backend_method_t::CONNECT},
        {"disconnect", backend_method_t::DISCONNECT},
        {"unloadMD", backend_method_t::UNLOAD_MD},
        {"prepXfer", backend_method_t::PREP_XFER},
        {"postXfer", backend_method_t::POST_XFER},
        {"checkXfer", backend_method_t::CHECK_XFER},
        {"releaseReqH", backend_method_t::RELEASE_REQ_H},
        {"prepMemView", backend_method_t::PREP_MEM_VIEW},
        {"getPublicData", backend_method_t::GET_PUBLIC_DATA},
        {"getConnInfo", backend_method_t::GET_CONN_INFO},
        {"loadRemoteConnInfo", backend_method_t::LOAD_REMOTE_CONN_INFO},
        {"loadRemoteMD", backend_method_t::LOAD_REMOTE_MD},
        {"loadLocalMD", backend_method_t::LOAD_LOCAL_MD},
        {"getNotifs", backend_method_t::GET_NOTIFS},
        {"genNotif", backend_method_t::GEN_NOTIF},
        {"queryMem", backend_method_t::QUERY_MEM},
        {"estimateXferCost", backend_method_t::ESTIMATE_XFER_COST},
    };

    constexpr std::pair<std::string_view, nixl_status_t> statuses[] = {
        {"NIXL_SUCCESS", NIXL_SUCCESS},
        {"NIXL_IN_PROG", NIXL_IN_PROG},
        {"NIXL_ERR_NOT_POSTED", NIXL_ERR_NOT_POSTED},
        {"NIXL_ERR_INVALID_PARAM", NIXL_ERR_INVALID_PARAM},
        {"NIXL_ERR_BACKEND", NIXL_ERR_BACKEND},
        {"NIXL_ERR_NOT_FOUND", NIXL_ERR_NOT_FOUND},
        {"NIXL_ERR_MISMATCH", NIXL_ERR_MISMATCH},
        {"NIXL_ERR_NOT_ALLOWED", NIXL_ERR_NOT_ALLOWED},
        {"NIXL_ERR_REPOST_ACTIVE", NIXL_ERR_REPOST_ACTIVE},
        {"NIXL_ERR_UNKNOWN", NIXL_ERR_UNKNOWN},
        {"NIXL_ERR_NOT_SUPPORTED", NIXL_ERR_NOT_SUPPORTED},
        {"NIXL_ERR_REMOTE_DISCONNECT", NIXL_ERR_REMOTE_DISCONNECT},
        {"NIXL_ERR_CANCELED", NIXL_ERR_CANCELED},
        {"NIXL_ERR_NO_TELEMETRY", NIXL_ERR_NO_TELEMETRY},
    };

    constexpr std::pair<std::string_view, nixl_xfer_op_t> operations[] = {
        {"NIXL_READ", NIXL_READ},
        {"NIXL_WRITE", NIXL_WRITE},
    };

    toml::table
    parseDocument(std::string_view text) {
        try {
            return toml::parse(text);
        }
        catch (const toml::parse_error &error) {
            std::ostringstream message;
            message << "Invalid fault configuration TOML: " << error;
            throw std::invalid_argument(message.str());
        }
    }

    // Opaque request handles expose no transfer operation; these methods take one directly.
    bool
    carriesOperation(backend_method_t method) {
        return method == backend_method_t::PREP_XFER || method == backend_method_t::POST_XFER ||
            method == backend_method_t::ESTIMATE_XFER_COST;
    }

    faultRule
    parseRule(const tableScope &fields) {
        fields.rejectUnknownKeys({"id", "method", "operation", "inject_on_occurrence", "status"});
        faultRule rule;
        rule.id = fields.requiredString("id");
        rule.method = fields.requiredSymbol("method", methods);
        rule.status = fields.requiredSymbol("status", statuses);
        rule.injectOnOccurrence = fields.positiveInteger("inject_on_occurrence");
        if (fields.contains("operation")) {
            if (!carriesOperation(rule.method)) {
                fields.reject("operation", "method has no transfer operation argument");
            }
            rule.operation = fields.requiredSymbol("operation", operations);
        }
        return rule;
    }

    // Two rules can match the same call when they name the same method and occurrence and
    // their operations intersect. An omitted operation covers both transfer directions.
    bool
    overlaps(const faultRule &left, const faultRule &right) {
        return left.method == right.method && left.injectOnOccurrence == right.injectOnOccurrence &&
            (!left.operation || !right.operation || *left.operation == *right.operation);
    }

    std::vector<faultRule>
    parseRules(const tableScope &section) {
        std::vector<faultRule> rules;
        const auto *array = section.optionalArray("rules");
        if (array == nullptr) {
            return rules;
        }
        std::unordered_set<std::string> identifiers;
        for (size_t index = 0; index < array->size(); ++index) {
            const auto element = "rules[" + std::to_string(index) + "]";
            const auto *table = (*array)[index].as_table();
            if (table == nullptr) {
                section.reject(element, "must be a table");
            }
            auto rule = parseRule(section.child(*table, element));
            if (!identifiers.insert(rule.id).second) {
                section.reject(element + ".id", "duplicate rule ID '" + rule.id + "'");
            }
            const auto conflict =
                std::find_if(rules.begin(), rules.end(), [&rule](const faultRule &earlier) {
                    return overlaps(rule, earlier);
                });
            if (conflict != rules.end()) {
                section.reject(element,
                               "overlaps rule '" + conflict->id + "': same method and occurrence");
            }
            rules.push_back(std::move(rule));
        }
        return rules;
    }

    std::string
    readFile(const std::filesystem::path &path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot open fault configuration: " + path.string());
        }
        // Opening a directory succeeds on common platforms, and only reading it fails.
        // std::istreambuf_iterator reads the stream buffer directly and never sets the
        // stream's own error bits, so checking file.bad() afterwards would miss that on
        // some standard libraries. Reject anything that is not a regular file instead.
        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status)) {
            throw std::runtime_error("Not a regular file, cannot read fault configuration: " +
                                     path.string());
        }
        try {
            return std::string(std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>());
        }
        catch (const std::ios_base::failure &error) {
            throw std::runtime_error("Cannot read fault configuration: " + path.string() + ": " +
                                     error.what());
        }
    }

} // namespace

faultConfig
faultConfig::parse(std::string_view text) {
    const auto document = parseDocument(text);
    const tableScope root(document, "");
    root.rejectUnknownKeys({"fault_injection"});
    const auto *table = document["fault_injection"].as_table();
    if (table == nullptr) {
        root.reject("fault_injection", "must be a table");
    }

    const auto section = root.child(*table, "fault_injection");
    section.rejectUnknownKeys({"schema_version", "experiment_id", "real_plugin_path", "rules"});
    if (section.requiredInteger("schema_version") != 1) {
        section.reject("schema_version", "must be integer 1");
    }

    faultConfig config;
    config.experimentId_ = section.requiredString("experiment_id");
    config.realPluginPath_ = section.requiredString("real_plugin_path");
    if (!config.realPluginPath_.is_absolute()) {
        section.reject("real_plugin_path", "must be an absolute path");
    }
    if (!config.realPluginPath_.has_filename()) {
        section.reject("real_plugin_path", "must name a file, not a directory");
    }
    config.rules_ = parseRules(section);
    return config;
}

faultConfig
faultConfig::load(const std::filesystem::path &path) {
    const auto text = readFile(path);
    try {
        return parse(text);
    }
    catch (const std::invalid_argument &error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    }
}

} // namespace nixl::fault
