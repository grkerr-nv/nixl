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

#include "rule_evaluator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using nixl::fault::backend_method_t;
using nixl::fault::faultConfig;
using nixl::fault::ruleEvaluator;

namespace {

faultConfig
makeConfig(std::string_view rules = {}) {
    return faultConfig::parse(std::string(R"([fault_injection]
schema_version = 1
experiment_id = "evaluator-test"
real_plugin_path = "/not-installed/libplugin_UCX.so"
)") + std::string(rules));
}

TEST(ruleEvaluatorTest, ForwardsWithoutRules) {
    ruleEvaluator evaluator(makeConfig());
    EXPECT_EQ(evaluator.evaluate(backend_method_t::CONNECT, std::nullopt), nullptr);
    EXPECT_EQ(evaluator.evaluate(backend_method_t::POST_XFER, NIXL_WRITE), nullptr);
}

TEST(ruleEvaluatorTest, CountsOnlyMatchingCallsAndFiresOnce) {
    ruleEvaluator evaluator(makeConfig(R"([[fault_injection.rules]]
id = "second-write"
method = "postXfer"
operation = "NIXL_WRITE"
inject_on_occurrence = 2
status = "NIXL_ERR_BACKEND"
)"));
    EXPECT_EQ(evaluator.evaluate(backend_method_t::CONNECT, std::nullopt), nullptr);
    EXPECT_EQ(evaluator.evaluate(backend_method_t::POST_XFER, NIXL_READ), nullptr);
    EXPECT_EQ(evaluator.evaluate(backend_method_t::POST_XFER, NIXL_WRITE), nullptr);
    const auto *rule = evaluator.evaluate(backend_method_t::POST_XFER, NIXL_WRITE);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "second-write");
    EXPECT_EQ(rule->status, NIXL_ERR_BACKEND);
    EXPECT_EQ(evaluator.evaluate(backend_method_t::POST_XFER, NIXL_WRITE), nullptr);
}

TEST(ruleEvaluatorTest, AdvancesEveryMatchingRuleWithIndependentRuntimeCounters) {
    const auto config = makeConfig(R"([[fault_injection.rules]]
id = "first"
method = "connect"
inject_on_occurrence = 1
status = "NIXL_SUCCESS"
[[fault_injection.rules]]
id = "third"
method = "connect"
inject_on_occurrence = 3
status = "NIXL_ERR_REMOTE_DISCONNECT"
)");
    ruleEvaluator first(config), second(config);
    const auto *rule = first.evaluate(backend_method_t::CONNECT, std::nullopt);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "first");
    EXPECT_EQ(rule->status, NIXL_SUCCESS);
    EXPECT_EQ(first.evaluate(backend_method_t::CONNECT, std::nullopt), nullptr);
    rule = first.evaluate(backend_method_t::CONNECT, std::nullopt);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "third");
    EXPECT_EQ(first.evaluate(backend_method_t::CONNECT, std::nullopt), nullptr);
    rule = second.evaluate(backend_method_t::CONNECT, std::nullopt);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "first");
}

TEST(ruleEvaluatorTest, WildcardAndSpecificRulesCountTheirOwnMatchingCalls) {
    const auto config = makeConfig(R"([[fault_injection.rules]]
id = "any-direction"
method = "prepXfer"
inject_on_occurrence = 1
status = "NIXL_ERR_CANCELED"
[[fault_injection.rules]]
id = "second-write"
method = "prepXfer"
operation = "NIXL_WRITE"
inject_on_occurrence = 2
status = "NIXL_ERR_BACKEND"
)");
    ruleEvaluator mixed(config), writes_only(config);
    const auto *rule = mixed.evaluate(backend_method_t::PREP_XFER, NIXL_READ);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "any-direction");
    EXPECT_EQ(mixed.evaluate(backend_method_t::PREP_XFER, NIXL_WRITE), nullptr);
    rule = mixed.evaluate(backend_method_t::PREP_XFER, NIXL_WRITE);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "second-write");
    rule = writes_only.evaluate(backend_method_t::PREP_XFER, NIXL_WRITE);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "any-direction");
    rule = writes_only.evaluate(backend_method_t::PREP_XFER, NIXL_WRITE);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->id, "second-write");
}

TEST(ruleEvaluatorTest, ConcurrentMatchingCallsFireEachRuleExactlyOnce) {
    // Eight threads share one evaluator and make 256 calls per method. Check that
    // connect occurrence 17 and disconnect occurrence 31 each select a rule exactly
    // once under contention, and that later calls leave both rules spent.
    constexpr int thread_count = 8;
    constexpr int calls_per_method_per_thread = 32;
    ruleEvaluator evaluator(makeConfig(R"([[fault_injection.rules]]
id = "seventeenth-connect"
method = "connect"
inject_on_occurrence = 17
status = "NIXL_ERR_BACKEND"
[[fault_injection.rules]]
id = "thirty-first-disconnect"
method = "disconnect"
inject_on_occurrence = 31
status = "NIXL_ERR_REMOTE_DISCONNECT"
)"));
    std::atomic<unsigned> connections{0}, disconnections{0};
    std::barrier start(thread_count);
    std::vector<std::jthread> workers;
    for (int index = 0; index < thread_count; ++index) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int call = 0; call < calls_per_method_per_thread; ++call) {
                if (evaluator.evaluate(backend_method_t::CONNECT, std::nullopt)) {
                    connections.fetch_add(1, std::memory_order_relaxed);
                }
                if (evaluator.evaluate(backend_method_t::DISCONNECT, std::nullopt)) {
                    disconnections.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    EXPECT_EQ(connections.load(), 1U);
    EXPECT_EQ(disconnections.load(), 1U);
    EXPECT_EQ(evaluator.evaluate(backend_method_t::CONNECT, std::nullopt), nullptr);
    EXPECT_EQ(evaluator.evaluate(backend_method_t::DISCONNECT, std::nullopt), nullptr);
}

} // namespace
