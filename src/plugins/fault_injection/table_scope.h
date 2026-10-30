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

#ifndef NIXL_SRC_PLUGINS_FAULT_INJECTION_TABLE_SCOPE_H
#define NIXL_SRC_PLUGINS_FAULT_INJECTION_TABLE_SCOPE_H

#include <toml++/toml.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace nixl::fault {

/** One TOML table plus the dotted field path that prefixes its error messages.
 *
 * Every accessor validates and throws std::invalid_argument on failure, so callers read
 * as a list of schema requirements rather than a chain of conditionals. Messages are
 * `<path><key>: <reason>`, where `child` accumulates the path.
 *
 * A scope refers to the caller's table and does not extend its lifetime.
 */
class tableScope {
public:
    tableScope(const toml::table &table, std::string path);

    /** Scope for a nested table, extending this path with key. */
    [[nodiscard]] tableScope
    child(const toml::table &table, std::string_view key) const;

    [[noreturn]] void
    reject(std::string_view key, std::string_view reason) const;

    void
    rejectUnknownKeys(std::initializer_list<std::string_view> allowed) const;

    [[nodiscard]] bool
    contains(std::string_view key) const;

    /** Null when the key is absent, so callers distinguish an omitted array from a
     * malformed one without reaching for the underlying table. Rejects a present value
     * that is not an array.
     */
    [[nodiscard]] const toml::array *
    optionalArray(std::string_view key) const;

    /** Rejects a missing, mistyped, empty, or control-character value. */
    [[nodiscard]] std::string
    requiredString(std::string_view key) const;

    /** Rejects a missing or mistyped value. */
    [[nodiscard]] int64_t
    requiredInteger(std::string_view key) const;

    /** Rejects zero and negative values alongside the requiredInteger failures. */
    [[nodiscard]] uint64_t
    positiveInteger(std::string_view key) const;

    /** Map a required string field onto one of symbols, reporting the accepted names. */
    template<typename valueType, size_t size>
    [[nodiscard]] valueType
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
    [[nodiscard]] std::string
    qualify(std::string_view key) const;

    const toml::table &table_;
    std::string path_;
};

} // namespace nixl::fault

#endif
