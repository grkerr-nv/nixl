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

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using nixl::fault::tableScope;

namespace {

// These tests assert whole rejection messages. The schema tests in fault_config_test only
// check that the offending field is named, so the wording of each reason is pinned here.
template<typename action_t>
std::string
rejection(action_t &&action) {
    try {
        action();
    }
    catch (const std::invalid_argument &error) {
        return error.what();
    }
    return "<accepted>";
}

TEST(tableScopeTest, QualifiesKeysWithTheAccumulatedPath) {
    const toml::table document = toml::parse(R"(
[section]
[section.nested]
)");
    const tableScope root(document, "");
    EXPECT_EQ(rejection([&] { root.reject("key", "reason"); }), "key: reason");

    const auto *section = document["section"].as_table();
    ASSERT_NE(section, nullptr);
    const auto scope = root.child(*section, "section");
    EXPECT_EQ(rejection([&] { scope.reject("key", "reason"); }), "section.key: reason");

    const auto *nested = (*section)["nested"].as_table();
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(rejection([&] { scope.child(*nested, "nested").reject("key", "reason"); }),
              "section.nested.key: reason");
}

TEST(tableScopeTest, DistinguishesMissingKeysFromMistypedOnes) {
    const toml::table document = toml::parse(R"(
name = 1
count = "text"
)");
    const tableScope scope(document, "");
    EXPECT_TRUE(scope.contains("name"));
    EXPECT_FALSE(scope.contains("absent"));
    EXPECT_EQ(rejection([&] { static_cast<void>(scope.requiredString("name")); }),
              "name: must be a string");
    EXPECT_EQ(rejection([&] { static_cast<void>(scope.requiredString("absent")); }),
              "absent: is required");
    EXPECT_EQ(rejection([&] { static_cast<void>(scope.requiredInteger("count")); }),
              "count: must be an integer");
    EXPECT_EQ(rejection([&] { static_cast<void>(scope.requiredInteger("absent")); }),
              "absent: is required");
}

TEST(tableScopeTest, AcceptsMultibyteTextAndRejectsEmptyOrControlCharacters) {
    const toml::table document = toml::parse(R"(
utf8 = "π-experiment"
empty = ""
newline = "two\nlines"
tab = "two\tfields"
deleted = "a\u007Fb"
)");
    const tableScope scope(document, "");
    // The control check tests raw bytes, so multibyte UTF-8 passes through unharmed.
    EXPECT_EQ(scope.requiredString("utf8"), "π-experiment");
    for (const auto *key : {"empty", "newline", "tab", "deleted"}) {
        SCOPED_TRACE(key);
        EXPECT_EQ(rejection([&] { static_cast<void>(scope.requiredString(key)); }),
                  std::string(key) + ": must be a nonempty string without control characters");
    }
}

TEST(tableScopeTest, AcceptsPositiveIntegersAndRejectsZeroAndNegative) {
    const toml::table document = toml::parse(R"(
one = 1
maximum = 9223372036854775807
zero = 0
negative = -1
)");
    const tableScope scope(document, "");
    EXPECT_EQ(scope.positiveInteger("one"), 1U);
    EXPECT_EQ(scope.positiveInteger("maximum"),
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    for (const auto *key : {"zero", "negative"}) {
        SCOPED_TRACE(key);
        EXPECT_EQ(rejection([&] { static_cast<void>(scope.positiveInteger(key)); }),
                  std::string(key) + ": must be a positive integer");
    }
}

TEST(tableScopeTest, ReturnsNullForAbsentArraysAndRejectsNonArrays) {
    const toml::table document = toml::parse(R"(
list = [1, 2]
scalar = 1
)");
    const tableScope scope(document, "");
    EXPECT_EQ(scope.optionalArray("absent"), nullptr);
    const auto *array = scope.optionalArray("list");
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(array->size(), 2U);
    EXPECT_EQ(rejection([&] { static_cast<void>(scope.optionalArray("scalar")); }),
              "scalar: must be an array of tables");
}

TEST(tableScopeTest, ReportsAcceptedSymbolsForUnknownValues) {
    const toml::table document = toml::parse(R"(
mode = "fast"
other = "sideways"
)");
    const tableScope scope(document, "");
    constexpr std::pair<std::string_view, int> symbols[] = {{"fast", 1}, {"slow", 2}};
    EXPECT_EQ(scope.requiredSymbol("mode", symbols), 1);
    EXPECT_EQ(rejection([&] { static_cast<void>(scope.requiredSymbol("other", symbols)); }),
              "other: unknown value 'sideways'; expected one of: fast, slow");
}

TEST(tableScopeTest, RejectsOnlyKeysOutsideTheAllowedList) {
    const toml::table document = toml::parse(R"(
kept = 1
surprise = 2
)");
    const tableScope scope(document, "");
    EXPECT_EQ(rejection([&] { scope.rejectUnknownKeys({"kept"}); }), "surprise: unknown key");
    EXPECT_EQ(rejection([&] { scope.rejectUnknownKeys({"kept", "surprise"}); }), "<accepted>");

    const toml::table empty = toml::parse("");
    EXPECT_EQ(rejection([&] { tableScope(empty, "").rejectUnknownKeys({}); }), "<accepted>");
}

} // namespace
