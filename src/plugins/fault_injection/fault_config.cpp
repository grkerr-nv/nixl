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

    // One TOML table plus the dotted field path that prefixes its error messages. Every
    // accessor validates and throws, so callers read as a list of schema requirements.
    class tableScope {
    public:
        tableScope(const toml::table &table, std::string path)
            : table_(table),
              path_(std::move(path)) {}

        // Scope for a nested table, extending this path with key.
        tableScope
        child(const toml::table &table, std::string_view key) const {
            return tableScope(table, qualify(key) + ".");
        }

        [[noreturn]] void
        reject(std::string_view key, std::string_view reason) const {
            throw std::invalid_argument(qualify(key) + ": " + std::string(reason));
        }

        void
        rejectUnknownKeys(std::initializer_list<std::string_view> allowed) const {
            for (const auto &[key, value] : table_) {
                if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
                    reject(key.str(), "unknown key");
                }
            }
        }

        bool
        contains(std::string_view key) const {
            return table_.contains(key);
        }

        toml::node_view<const toml::node>
        operator[](std::string_view key) const {
            return table_[key];
        }

        std::string
        requiredString(std::string_view key) const {
            const auto value = table_[key].value<std::string>();
            if (!value) {
                reject(key, contains(key) ? "must be a string" : "is required");
            }
            if (value->empty() || value->find('\0') != std::string::npos) {
                reject(key, "must be a nonempty string without NUL bytes");
            }
            return *value;
        }

        int64_t
        requiredInteger(std::string_view key) const {
            if (!table_[key].is_integer()) {
                reject(key, contains(key) ? "must be an integer" : "is required");
            }
            return *table_[key].value<int64_t>();
        }

        uint64_t
        positiveInteger(std::string_view key) const {
            const auto value = requiredInteger(key);
            if (value <= 0) {
                reject(key, "must be a positive integer");
            }
            return static_cast<uint64_t>(value);
        }

        // Map a required string field onto one of symbols, reporting the accepted names.
        template<typename valueType, size_t size>
        valueType
        requiredSymbol(std::string_view key,
                       const std::pair<std::string_view, valueType> (&symbols)[size]) const {
            const auto value = requiredString(key);
            for (const auto &[name, result] : symbols) {
                if (value == name) {
                    return result;
                }
            }
            std::string accepted;
            for (const auto &[name, result] : symbols) {
                accepted += accepted.empty() ? "" : ", ";
                accepted += name;
            }
            reject(key, "unknown value '" + value + "'; expected one of: " + accepted);
        }

    private:
        std::string
        qualify(std::string_view key) const {
            return path_ + std::string(key);
        }

        const toml::table &table_;
        std::string path_;
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
        if (!section["rules"]) {
            return rules;
        }
        const auto *array = section["rules"].as_array();
        if (array == nullptr) {
            section.reject("rules", "must be an array of tables");
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
        return text;
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
