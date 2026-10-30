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

#include "table_scope.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nixl::fault {

tableScope::tableScope(const toml::table &table, std::string path)
    : table_(table),
      path_(std::move(path)) {}

tableScope
tableScope::child(const toml::table &table, std::string_view key) const {
    return tableScope(table, qualify(key) + ".");
}

void
tableScope::reject(std::string_view key, std::string_view reason) const {
    throw std::invalid_argument(qualify(key) + ": " + std::string(reason));
}

void
tableScope::rejectUnknownKeys(std::initializer_list<std::string_view> allowed) const {
    for (const auto &[key, value] : table_) {
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
            reject(key.str(), "unknown key");
        }
    }
}

bool
tableScope::contains(std::string_view key) const {
    return table_.contains(key);
}

const toml::array *
tableScope::optionalArray(std::string_view key) const {
    if (!contains(key)) {
        return nullptr;
    }
    const auto *array = table_[key].as_array();
    if (array == nullptr) {
        reject(key, "must be an array of tables");
    }
    return array;
}

std::string
tableScope::requiredString(std::string_view key) const {
    const auto value = table_[key].value<std::string>();
    if (!value) {
        reject(key, contains(key) ? "must be a string" : "is required");
    }
    // Control bytes would corrupt the error messages and logs that carry these values.
    // Testing bytes rather than std::iscntrl keeps this locale-independent and leaves
    // multibyte UTF-8 intact.
    const auto control = [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7F;
    };
    if (value->empty() || std::any_of(value->begin(), value->end(), control)) {
        reject(key, "must be a nonempty string without control characters");
    }
    return *value;
}

int64_t
tableScope::requiredInteger(std::string_view key) const {
    if (!table_[key].is_integer()) {
        reject(key, contains(key) ? "must be an integer" : "is required");
    }
    return *table_[key].value<int64_t>();
}

uint64_t
tableScope::positiveInteger(std::string_view key) const {
    const auto value = requiredInteger(key);
    if (value <= 0) {
        reject(key, "must be a positive integer");
    }
    return static_cast<uint64_t>(value);
}

std::string
tableScope::qualify(std::string_view key) const {
    return path_ + std::string(key);
}

} // namespace nixl::fault
