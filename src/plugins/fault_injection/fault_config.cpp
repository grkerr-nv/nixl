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

#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

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

    [[noreturn]] void
    invalidField(const std::string &field, std::string_view reason) {
        throw std::invalid_argument(field + ": " + std::string(reason));
    }

    void
    checkKeys(const toml::table &table,
              std::initializer_list<std::string_view> allowed,
              const std::string &context) {
        for (const auto &[key, value] : table) {
            if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
                invalidField(context + std::string(key.str()), "unknown key");
            }
        }
    }

    std::string
    requiredString(const toml::table &table, std::string_view key, const std::string &context) {
        const auto value = table[key].value<std::string>();
        if (!value || value->empty() || value->find('\0') != std::string::npos) {
            invalidField(context + std::string(key), "must be a nonempty string without NUL bytes");
        }
        return *value;
    }

    template<typename valueType, size_t size>
    valueType
    parseSymbol(const std::string &value,
                const std::pair<std::string_view, valueType> (&symbols)[size],
                const std::string &field) {
        for (const auto &[name, result] : symbols) {
            if (value == name) {
                return result;
            }
        }
        invalidField(field, "unknown value '" + value + "'");
    }

    faultRule
    parseRule(const toml::table &table, const std::string &context) {
        checkKeys(table, {"id", "method", "operation", "inject_on_occurrence", "status"}, context);
        faultRule rule;
        rule.id = requiredString(table, "id", context);
        rule.method =
            parseSymbol(requiredString(table, "method", context), methods, context + "method");
        rule.status =
            parseSymbol(requiredString(table, "status", context), statuses, context + "status");
        const auto occurrence = table["inject_on_occurrence"];
        if (!occurrence.is_integer() || occurrence.value<int64_t>().value() <= 0) {
            invalidField(context + "inject_on_occurrence", "must be a positive integer");
        }
        rule.injectOnOccurrence = static_cast<uint64_t>(*occurrence.value<int64_t>());
        if (table.contains("operation")) {
            rule.operation = parseSymbol(
                requiredString(table, "operation", context), operations, context + "operation");
            // These methods carry an operation directly; opaque request handles do not expose one.
            if (rule.method != backend_method_t::PREP_XFER &&
                rule.method != backend_method_t::POST_XFER &&
                rule.method != backend_method_t::ESTIMATE_XFER_COST) {
                invalidField(context + "operation", "method has no transfer operation argument");
            }
        }
        return rule;
    }

} // namespace

faultConfig
faultConfig::parse(std::string_view text) {
    toml::table document;
    try {
        document = toml::parse(text);
    }
    catch (const toml::parse_error &error) {
        std::ostringstream message;
        message << "Invalid fault configuration TOML: " << error;
        throw std::invalid_argument(message.str());
    }
    checkKeys(document, {"fault_injection"}, "");
    const auto *section = document["fault_injection"].as_table();
    if (!section) {
        invalidField("fault_injection", "must be a table");
    }
    const std::string context = "fault_injection.";
    checkKeys(*section, {"schema_version", "experiment_id", "real_plugin_path", "rules"}, context);
    const auto version = (*section)["schema_version"];
    if (!version.is_integer() || version.value<int64_t>() != 1) {
        invalidField(context + "schema_version", "must be integer 1");
    }
    faultConfig config;
    config.experimentId_ = requiredString(*section, "experiment_id", context);
    const auto path = requiredString(*section, "real_plugin_path", context);
    config.realPluginPath_ = path;
    if (!config.realPluginPath_.is_absolute()) {
        invalidField(context + "real_plugin_path", "must be an absolute path");
    }
    const auto rules = (*section)["rules"];
    if (!rules) {
        return config;
    }
    const auto *array = rules.as_array();
    if (!array) {
        invalidField(context + "rules", "must be an array of tables");
    }
    std::unordered_set<std::string> identifiers;
    for (size_t index = 0; index < array->size(); ++index) {
        const auto location = context + "rules[" + std::to_string(index) + "].";
        const auto *table = (*array)[index].as_table();
        if (!table) {
            invalidField(context + "rules[" + std::to_string(index) + "]", "must be a table");
        }
        auto rule = parseRule(*table, location);
        if (!identifiers.insert(rule.id).second) {
            invalidField(location + "id", "duplicate rule ID '" + rule.id + "'");
        }
        config.rules_.push_back(std::move(rule));
    }
    return config;
}

faultConfig
faultConfig::load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open fault configuration: " + path.string());
    }
    std::string text;
    try {
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    catch (const std::ios_base::failure &error) {
        throw std::runtime_error("Cannot read fault configuration: " + path.string() + ": " +
                                 error.what());
    }
    if (file.bad()) {
        throw std::runtime_error("Cannot read fault configuration: " + path.string());
    }
    try {
        return parse(text);
    }
    catch (const std::invalid_argument &error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    }
}

} // namespace nixl::fault
