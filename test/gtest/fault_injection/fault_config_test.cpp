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

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using nixl::fault::backend_method_t;
using nixl::fault::faultConfig;

namespace {

constexpr std::string_view config_text = R"([fault_injection]
schema_version = 1
experiment_id = "first-write"
real_plugin_path = "/not-installed/libplugin_UCX.so"
)";

constexpr std::string_view rule_text = R"([[fault_injection.rules]]
id = "fail-first-post"
method = "postXfer"
operation = "NIXL_WRITE"
inject_on_occurrence = 1
status = "NIXL_ERR_BACKEND"
)";

void
expectInvalid(std::string_view text, std::string_view field) {
    try {
        static_cast<void>(faultConfig::parse(text));
        FAIL() << "Accepted invalid configuration: " << text;
    }
    catch (const std::invalid_argument &error) {
        EXPECT_NE(std::string_view(error.what()).find(field), std::string_view::npos)
            << error.what();
    }
}

TEST(faultConfigTest, OwnsParsedFieldsAndPreservesRuleOrder) {
    std::string text = std::string(config_text) + std::string(rule_text) + R"(
[[fault_injection.rules]]
id = "fail-check"
method = "checkXfer"
inject_on_occurrence = 9223372036854775807
status = "NIXL_ERR_REMOTE_DISCONNECT"
)";
    const auto config = faultConfig::parse(text);
    text.clear();
    EXPECT_EQ(config.getExperimentId(), "first-write");
    EXPECT_EQ(config.getRealPluginPath(), "/not-installed/libplugin_UCX.so");
    ASSERT_EQ(config.getRules().size(), 2U);
    const auto &first = config.getRules()[0];
    EXPECT_EQ(first.id, "fail-first-post");
    EXPECT_EQ(first.method, backend_method_t::POST_XFER);
    EXPECT_EQ(first.operation, NIXL_WRITE);
    EXPECT_EQ(first.injectOnOccurrence, 1U);
    EXPECT_EQ(first.status, NIXL_ERR_BACKEND);
    const auto &second = config.getRules()[1];
    EXPECT_EQ(second.id, "fail-check");
    EXPECT_EQ(second.method, backend_method_t::CHECK_XFER);
    EXPECT_FALSE(second.operation.has_value());
    EXPECT_EQ(second.injectOnOccurrence, std::numeric_limits<int64_t>::max());
    EXPECT_EQ(second.status, NIXL_ERR_REMOTE_DISCONNECT);
}

TEST(faultConfigTest, AcceptsOmittedAndEmptyRules) {
    EXPECT_TRUE(faultConfig::parse(config_text).getRules().empty());
    EXPECT_TRUE(faultConfig::parse(std::string(config_text) + "rules = []\n").getRules().empty());
}

TEST(faultConfigTest, AcceptsCompleteStatusVocabulary) {
    const std::pair<std::string_view, nixl_status_t> statuses[] = {
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
    for (const auto &[name, status] : statuses) {
        SCOPED_TRACE(name);
        const auto text = std::string(config_text) + R"([[fault_injection.rules]]
id = "status-check"
method = "registerMem"
inject_on_occurrence = 1
status = ")" +
            std::string(name) + "\"\n";
        const auto config = faultConfig::parse(text);
        ASSERT_EQ(config.getRules().size(), 1U);
        EXPECT_EQ(config.getRules()[0].status, status);
        EXPECT_EQ(config.getRules()[0].method, backend_method_t::REGISTER_MEM);
    }
}

TEST(faultConfigTest, RejectsInvalidStructureAndTypesWithFieldContext) {
    expectInvalid("invalid TOML !", "TOML");
    expectInvalid("", "fault_injection");
    expectInvalid("fault_injection = 1", "fault_injection");
    expectInvalid(std::string(config_text) + "rules = {}\n", "fault_injection.rules");
    expectInvalid(std::string(config_text) + "rules = [1]\n", "fault_injection.rules[0]");
    const std::pair<std::string_view, std::string_view> replacements[] = {
        {"schema_version = 1", "schema_version = 2"},
        {"schema_version = 1", "schema_version = 1.0"},
        {"schema_version = 1", "schema_version = true"},
        {"schema_version = 1", ""},
        {"experiment_id = \"first-write\"", "experiment_id = \"\""},
        {"experiment_id = \"first-write\"", "experiment_id = 1"},
        {"real_plugin_path = \"/not-installed/libplugin_UCX.so\"",
         "real_plugin_path = 'relative.so'"},
        {"real_plugin_path = \"/not-installed/libplugin_UCX.so\"", "real_plugin_path = \"\""},
        {"real_plugin_path = \"/not-installed/libplugin_UCX.so\"",
         "real_plugin_path = \"/tmp/a\\u0000b\""},
    };
    for (const auto &[original, replacement] : replacements) {
        SCOPED_TRACE(replacement);
        std::string text(config_text);
        text.replace(text.find(original), original.size(), replacement);
        expectInvalid(text, original.substr(0, original.find(' ')));
    }
}

TEST(faultConfigTest, RejectsInvalidRuleFieldsWithRuleIndex) {
    const std::pair<std::string_view, std::string_view> replacements[] = {
        {"id = \"fail-first-post\"", "id = \"\""},
        {"id = \"fail-first-post\"", "id = 1"},
        {"method = \"postXfer\"", "method = \"unknownMethod\""},
        {"method = \"postXfer\"", "method = \"releaseMemView\""},
        {"operation = \"NIXL_WRITE\"", "operation = \"WRITE\""},
        {"operation = \"NIXL_WRITE\"", "operation = false"},
        {"inject_on_occurrence = 1", "inject_on_occurrence = 0"},
        {"inject_on_occurrence = 1", "inject_on_occurrence = -1"},
        {"inject_on_occurrence = 1", "inject_on_occurrence = 1.0"},
        {"inject_on_occurrence = 1", "inject_on_occurrence = true"},
        {"inject_on_occurrence = 1", ""},
        {"status = \"NIXL_ERR_BACKEND\"", "status = \"NIXL_ERR_FUTURE\""},
        {"status = \"NIXL_ERR_BACKEND\"", "status = -3"},
        {"status = \"NIXL_ERR_BACKEND\"", ""},
    };
    for (const auto &[original, replacement] : replacements) {
        SCOPED_TRACE(replacement);
        std::string text = std::string(config_text) + std::string(rule_text);
        text.replace(text.find(original), original.size(), replacement);
        const auto field = original.substr(0, original.find(' '));
        expectInvalid(text, "fault_injection.rules[0]." + std::string(field));
    }
}

TEST(faultConfigTest, RejectsUnknownKeysAndDuplicateRuleIds) {
    expectInvalid("unexpected = 1\n" + std::string(config_text), "unexpected");
    expectInvalid(std::string(config_text) + "unexpected = 1\n", "fault_injection.unexpected");
    expectInvalid(std::string(config_text) + std::string(rule_text) + "request_handle = 'null'\n",
                  "fault_injection.rules[0].request_handle");
    expectInvalid(std::string(config_text) + std::string(rule_text) + std::string(rule_text),
                  "fault_injection.rules[1].id: duplicate rule ID");
}

TEST(faultConfigTest, RejectsRulesThatCanMatchTheSameCall) {
    // rule_text targets postXfer, NIXL_WRITE, occurrence 1. Omitting the operation covers
    // both directions, so the second rule can match the same call.
    const std::string base = std::string(config_text) + std::string(rule_text);
    expectInvalid(base + R"([[fault_injection.rules]]
id = "any-direction"
method = "postXfer"
inject_on_occurrence = 1
status = "NIXL_ERR_CANCELED"
)",
                  "fault_injection.rules[1]: overlaps rule 'fail-first-post'");

    // A disjoint operation and a later occurrence stay independent.
    const auto config = faultConfig::parse(base + R"([[fault_injection.rules]]
id = "other-direction"
method = "postXfer"
operation = "NIXL_READ"
inject_on_occurrence = 1
status = "NIXL_ERR_CANCELED"
[[fault_injection.rules]]
id = "second-write"
method = "postXfer"
operation = "NIXL_WRITE"
inject_on_occurrence = 2
status = "NIXL_ERR_CANCELED"
)");
    EXPECT_EQ(config.getRules().size(), 3U);
}

TEST(faultConfigTest, AllowsOperationOnlyOnMethodsWithAnOperationArgument) {
    for (const auto method : {"prepXfer", "postXfer", "estimateXferCost", "checkXfer", "connect"}) {
        SCOPED_TRACE(method);
        std::string text = std::string(config_text) + std::string(rule_text);
        text.replace(text.find("postXfer"), std::string_view("postXfer").size(), method);
        if (std::string_view(method) == "checkXfer" || std::string_view(method) == "connect") {
            expectInvalid(text, "fault_injection.rules[0].operation");
        } else {
            EXPECT_EQ(faultConfig::parse(text).getRules()[0].operation, NIXL_WRITE);
        }
    }
}

class faultConfigFileTest : public testing::Test {
protected:
    void
    SetUp() override {
        char directory_template[] = "/tmp/nixl-fault-config-XXXXXX";
        const char *created = mkdtemp(directory_template);
        ASSERT_NE(created, nullptr);
        directory = created;
        configPath = directory / "fault.toml";
    }

    void
    TearDown() override {
        if (!directory.empty()) {
            std::filesystem::remove_all(directory);
        }
    }

    std::filesystem::path directory;
    std::filesystem::path configPath;
};

TEST_F(faultConfigFileTest, LoadsSnapshotAndReportsFileAndSchemaFailures) {
    EXPECT_THROW(static_cast<void>(faultConfig::load(configPath)), std::runtime_error);
    try {
        static_cast<void>(faultConfig::load(directory));
        FAIL() << "Accepted a directory as a configuration file";
    }
    catch (const std::runtime_error &error) {
        EXPECT_NE(std::string_view(error.what()).find(directory.string()), std::string_view::npos);
    }
    {
        std::ofstream file(configPath);
        ASSERT_TRUE(file);
        file << config_text << rule_text;
    }
    const auto config = faultConfig::load(configPath);
    ASSERT_EQ(config.getRules().size(), 1U);
    EXPECT_EQ(config.getRules()[0].method, backend_method_t::POST_XFER);
    {
        std::ofstream file(configPath);
        ASSERT_TRUE(file);
        file << "[fault_injection]\nschema_version = 2\n";
    }
    EXPECT_EQ(config.getRules()[0].id, "fail-first-post");
    try {
        static_cast<void>(faultConfig::load(configPath));
        FAIL() << "Accepted invalid schema";
    }
    catch (const std::invalid_argument &error) {
        EXPECT_NE(std::string_view(error.what()).find(configPath.string()), std::string_view::npos);
        EXPECT_NE(std::string_view(error.what()).find("schema_version"), std::string_view::npos);
    }
}

} // namespace
