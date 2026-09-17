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

#include <filesystem>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "backend_plugin_loader.h"
#include "common.h"
#include "mocks/gmock_engine.h"

namespace {

using testing::ElementsAre;
using testing::NiceMock;
using testing::Return;

const std::filesystem::path mock_plugin_path =
    std::filesystem::path(BUILD_DIR) / "test/gtest/mocks/libplugin_MOCK_BACKEND.so";
const std::filesystem::path fixture_dir =
    std::filesystem::path(BUILD_DIR) / "test/gtest/fault_injection";

TEST(backendPluginLoaderTest, RejectsNonAbsolutePath) {
    const gtest::LogIgnoreGuard ignore("Backend plugin path must be absolute");

    EXPECT_EQ(nixlBackendPluginLoader::load("libplugin_MOCK_BACKEND.so"), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsMismatchedApiVersion) {
    const gtest::LogIgnoreGuard ignore("Backend plugin API version mismatch");
    const auto path = fixture_dir / "libbackend_plugin_bad_api_fixture.so";

    EXPECT_EQ(nixlBackendPluginLoader::load(path), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsMissingRequiredCallback) {
    const gtest::LogIgnoreGuard ignore("is missing a required callback");
    const auto path = fixture_dir / "libbackend_plugin_missing_callback_fixture.so";

    EXPECT_EQ(nixlBackendPluginLoader::load(path), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, LoadsPluginMetadata) {
    auto loader = nixlBackendPluginLoader::load(mock_plugin_path);

    ASSERT_NE(loader, nullptr);
    EXPECT_EQ(loader->getName(), gtest::GetMockBackendName());
    EXPECT_EQ(loader->getVersion(), "0.0.1");
    EXPECT_TRUE(loader->getBackendOptions().empty());
    EXPECT_THAT(loader->getBackendMems(), ElementsAre(DRAM_SEG));
}

TEST(backendPluginLoaderTest, EngineKeepsPluginLoaded) {
    NiceMock<mocks::GMockBackendEngine> mock_engine;
    nixl_b_params_t custom_params;
    mock_engine.SetToParams(custom_params);
    nixlBackendInitParams init_params{
        .localAgent = "local", .type = gtest::GetMockBackendName(), .customParams = &custom_params};

    auto loader = nixlBackendPluginLoader::load(mock_plugin_path);
    ASSERT_NE(loader, nullptr);
    auto engine = loader->createEngine(&init_params);
    ASSERT_NE(engine, nullptr);

    EXPECT_CALL(mock_engine, supportsRemote()).WillOnce(Return(false));
    loader.reset();

    EXPECT_FALSE(engine->supportsRemote());
    engine.reset();
}

} // namespace
