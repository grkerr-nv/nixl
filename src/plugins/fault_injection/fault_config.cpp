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

#include <absl/crc/crc32c.h>
#include <toml++/toml.hpp>

#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nixl::fault {

bool
carriesOperation(backend_method_t method) {
    return method == backend_method_t::PREP_XFER || method == backend_method_t::POST_XFER ||
        method == backend_method_t::ESTIMATE_XFER_COST;
}

bool
writesOutputOnSuccess(backend_method_t method) {
    // Classify by the method's own signature. The true arm is every method whose success
    // contract fills a non-const reference argument that core then reads, whether that is a
    // handle, a metadata pointer, a string, or a measurement. The false arm is the methods whose
    // returned status is the whole answer; checkXfer is the clearest of those, and injecting
    // NIXL_SUCCESS on it is a real fault rather than a lie, because it reports a transfer as
    // finished.
    //
    // getNotifs is the deliberate exception, an output argument sitting in the false arm. Core
    // clears the list before every call, so a call that writes nothing reports no notifications
    // rather than stale ones, which makes an injected success a dropped-notification fault.
    switch (method) {
    case backend_method_t::REGISTER_MEM:
    case backend_method_t::PREP_XFER:
    case backend_method_t::POST_XFER:
    case backend_method_t::PREP_MEM_VIEW:
    case backend_method_t::GET_PUBLIC_DATA:
    case backend_method_t::GET_CONN_INFO:
    case backend_method_t::LOAD_REMOTE_MD:
    case backend_method_t::LOAD_LOCAL_MD:
    case backend_method_t::QUERY_MEM:
    case backend_method_t::ESTIMATE_XFER_COST:
        return true;
    case backend_method_t::DEREGISTER_MEM:
    case backend_method_t::CONNECT:
    case backend_method_t::DISCONNECT:
    case backend_method_t::UNLOAD_MD:
    case backend_method_t::CHECK_XFER:
    case backend_method_t::RELEASE_REQ_H:
    case backend_method_t::LOAD_REMOTE_CONN_INFO:
    case backend_method_t::GET_NOTIFS:
    case backend_method_t::GEN_NOTIF:
        return false;
    }
    return false;
}

bool
isErrorStatus(nixl_status_t status) {
    switch (status) {
    case NIXL_SUCCESS:
    case NIXL_IN_PROG:
        return false;
    case NIXL_ERR_NOT_POSTED:
    case NIXL_ERR_INVALID_PARAM:
    case NIXL_ERR_BACKEND:
    case NIXL_ERR_NOT_FOUND:
    case NIXL_ERR_MISMATCH:
    case NIXL_ERR_NOT_ALLOWED:
    case NIXL_ERR_REPOST_ACTIVE:
    case NIXL_ERR_UNKNOWN:
    case NIXL_ERR_NOT_SUPPORTED:
    case NIXL_ERR_REMOTE_DISCONNECT:
    case NIXL_ERR_CANCELED:
    case NIXL_ERR_NO_TELEMETRY:
        return true;
    }
    // Unreachable for the statuses a rule can name, which the table above fixes. An
    // unclassified value counts as a non-error so it meets the output check rather than
    // slipping past it.
    return false;
}

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

    faultRule
    parseRule(const tableScope &fields) {
        fields.rejectUnknownKeysAndThrow(
            {"id", "method", "operation", "inject_on_occurrence", "status"});
        faultRule rule;
        rule.id = fields.requiredString("id");
        rule.method = fields.requiredSymbol("method", methods);
        rule.status = fields.requiredSymbol("status", statuses);
        // Both non-error statuses promise core a result the suppressed call never produced.
        if (!isErrorStatus(rule.status) && writesOutputOnSuccess(rule.method)) {
            fields.rejectAndThrow("status",
                                  std::string(statusName(rule.status)) + " would leave " +
                                      std::string(methodName(rule.method)) +
                                      "'s output argument unwritten, and injection cannot "
                                      "fabricate one; this method takes error statuses only");
        }
        rule.injectOnOccurrence = fields.positiveInteger("inject_on_occurrence");
        if (fields.contains("operation")) {
            if (!carriesOperation(rule.method)) {
                fields.rejectAndThrow("operation", "method has no transfer operation argument");
            }
            rule.operation = fields.requiredSymbol("operation", operations);
        }
        return rule;
    }

    // Two rules collide when they can count the same call. Equal selectors always do. A
    // rule that omits the operation counts both directions, so its occurrence can land on
    // a directional rule's earlier one even though the two numbers differ. Returns why,
    // or an empty string when the rules are independent.
    std::string
    overlapReason(const faultRule &rule, const faultRule &earlier) {
        if (rule.method != earlier.method) {
            return {};
        }
        if (rule.operation == earlier.operation) {
            if (rule.injectOnOccurrence != earlier.injectOnOccurrence) {
                return {};
            }
            return "same method, operation and occurrence";
        }
        if (rule.operation && earlier.operation) {
            // Two different directions never count the same call.
            return {};
        }
        // Exactly one of the two omits the operation.
        const faultRule &wildcard = rule.operation ? earlier : rule;
        const faultRule &directional = rule.operation ? rule : earlier;
        if (wildcard.injectOnOccurrence < directional.injectOnOccurrence) {
            return {};
        }
        return "the rule without an operation counts both directions, so its occurrence " +
            std::to_string(wildcard.injectOnOccurrence) + " can be occurrence " +
            std::to_string(directional.injectOnOccurrence) + " of the directional rule";
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
                section.rejectAndThrow(element, "must be a table");
            }
            auto rule = parseRule(section.child(*table, element));
            if (!identifiers.insert(rule.id).second) {
                section.rejectAndThrow(element + ".id", "duplicate rule ID '" + rule.id + "'");
            }
            for (const auto &earlier : rules) {
                const auto reason = overlapReason(rule, earlier);
                if (!reason.empty()) {
                    section.rejectAndThrow(element,
                                           "overlaps rule '" + earlier.id + "': " + reason);
                }
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

    // These tables and the fallback reference string literals; returned views stay
    // valid while this library remains loaded.
    template<class T, size_t N>
    std::string_view
    symbolName(T symbol, const std::pair<std::string_view, T> (&symbols)[N]) {
        for (const auto &[name, value] : symbols) {
            if (value == symbol) {
                return name;
            }
        }
        return "unknown";
    }

} // namespace

std::string_view
methodName(backend_method_t method) {
    return symbolName(method, methods);
}

std::string_view
statusName(nixl_status_t status) {
    return symbolName(status, statuses);
}

std::string_view
operationName(nixl_xfer_op_t operation) {
    return symbolName(operation, operations);
}

faultConfig
faultConfig::parse(std::string_view text) {
    const auto document = parseDocument(text);
    const tableScope root(document, "");
    root.rejectUnknownKeysAndThrow({"fault_injection"});
    const auto *table = document["fault_injection"].as_table();
    if (table == nullptr) {
        root.rejectAndThrow("fault_injection", "must be a table");
    }

    const auto section = root.child(*table, "fault_injection");
    section.rejectUnknownKeysAndThrow(
        {"schema_version", "experiment_id", "real_plugin_path", "rules"});
    if (section.requiredInteger("schema_version") != 1) {
        section.rejectAndThrow("schema_version", "must be integer 1");
    }

    faultConfig config;
    config.experimentId_ = section.requiredString("experiment_id");
    config.realPluginPath_ = section.requiredString("real_plugin_path");
    if (!config.realPluginPath_.is_absolute()) {
        section.rejectAndThrow("real_plugin_path", "must be an absolute path");
    }
    if (!config.realPluginPath_.has_filename()) {
        section.rejectAndThrow("real_plugin_path", "must name a file, not a directory");
    }
    config.rules_ = parseRules(section);
    const auto digest = static_cast<uint32_t>(absl::ComputeCrc32c(text));
    std::ostringstream fingerprint;
    fingerprint << "crc32c:" << std::hex << std::setfill('0') << std::setw(8) << digest;
    config.digest_ = fingerprint.str();
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
