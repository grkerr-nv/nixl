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

#include <dlfcn.h>

#include <filesystem>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "backend_plugin_fixture_dir.h"
#include "backend_plugin_loader.h"
#include "common.h"
#include "mocks/gmock_engine.h"

namespace {

using testing::ElementsAre;
using testing::IsEmpty;
using testing::NiceMock;
using testing::Return;

const std::filesystem::path mock_plugin_path =
    std::filesystem::path(BUILD_DIR) / "test/gtest/mocks/libplugin_MOCK_BACKEND.so";

std::filesystem::path
fixturePath(const std::string &variant) {
    return std::filesystem::path(NIXL_TEST_PLUGIN_FIXTURE_DIR) /
        ("libbackend_plugin_" + variant + "_fixture.so");
}

/**
 * Reads a fixture's nixl_plugin_fini counter through a handle of its own, so
 * finalization can be observed without depending on the loader's handle.
 */
class finiCounter {
public:
    explicit finiCounter(const std::filesystem::path &path)
        : handle_(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)) {
        if (handle_ != nullptr) {
            count_ =
                reinterpret_cast<unsigned (*)()>(dlsym(handle_, "nixl_test_plugin_fini_count"));
        }
    }

    ~finiCounter() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    finiCounter(const finiCounter &) = delete;
    void
    operator=(const finiCounter &) = delete;

    bool
    isUsable() const {
        return count_ != nullptr;
    }

    unsigned
    operator()() const {
        return count_();
    }

private:
    void *handle_;
    unsigned (*count_)() = nullptr;
};

TEST(backendPluginLoaderTest, RejectsNonAbsolutePath) {
    const gtest::LogIgnoreGuard ignore("Backend plugin path must be absolute");

    EXPECT_EQ(nixlBackendPluginLoader::load("libplugin_MOCK_BACKEND.so"), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsMissingFile) {
    const gtest::LogIgnoreGuard ignore("Failed to load backend plugin");

    EXPECT_EQ(nixlBackendPluginLoader::load(fixturePath("absent")), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsPluginWithoutInitSymbol) {
    const gtest::LogIgnoreGuard ignore("Failed to find nixl_plugin_init");

    EXPECT_EQ(nixlBackendPluginLoader::load(fixturePath("no_init")), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsPluginWhoseInitReturnsNull) {
    const gtest::LogIgnoreGuard ignore("Backend plugin initialization failed");

    EXPECT_EQ(nixlBackendPluginLoader::load(fixturePath("null_init")), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsMismatchedApiVersion) {
    const gtest::LogIgnoreGuard ignore("Backend plugin API version mismatch");

    EXPECT_EQ(nixlBackendPluginLoader::load(fixturePath("bad_api")), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, RejectsMissingRequiredCallback) {
    const gtest::LogIgnoreGuard ignore("is missing a required callback");

    EXPECT_EQ(nixlBackendPluginLoader::load(fixturePath("missing_callback")), nullptr);
    EXPECT_EQ(ignore.getIgnoredCount(), 1);
}

TEST(backendPluginLoaderTest, AcceptsPluginWithoutOptionalCallbacks) {
    auto loader = nixlBackendPluginLoader::load(fixturePath("no_optional"));

    ASSERT_NE(loader, nullptr);
    EXPECT_EQ(loader->getName(), "PLUGIN_LOADER_FIXTURE");
    EXPECT_THAT(loader->getBackendOptions(), IsEmpty());
    EXPECT_THAT(loader->getBackendMems(), IsEmpty());
}

TEST(backendPluginLoaderTest, ReportsEngineCreationFailure) {
    nixl_b_params_t custom_params;
    const nixlBackendInitParams init_params{
        .localAgent = "local", .type = "PLUGIN_LOADER_FIXTURE", .customParams = &custom_params};

    auto loader = nixlBackendPluginLoader::load(fixturePath("null_engine"));
    ASSERT_NE(loader, nullptr);

    EXPECT_EQ(loader->createEngine(&init_params), nullptr);
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

TEST(backendPluginLoaderTest, FinalizesOnlyAfterTheLastEngineIsReleased) {
    const auto path = fixturePath("good");
    const finiCounter finalized(path);
    ASSERT_TRUE(finalized.isUsable());
    // Plugins stay mapped for the life of the process, so the count is
    // whatever earlier tests left behind.
    const unsigned before = finalized();

    nixl_b_params_t custom_params;
    const nixlBackendInitParams init_params{
        .localAgent = "local", .type = "PLUGIN_LOADER_FIXTURE", .customParams = &custom_params};

    {
        auto loader = nixlBackendPluginLoader::load(path);
        ASSERT_NE(loader, nullptr);
        auto first = loader->createEngine(&init_params);
        auto second = loader->createEngine(&init_params);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);

        loader.reset();
        EXPECT_EQ(finalized(), before) << "dropping the loader finalized the plugin";

        {
            // The plugin is held by the deleter, so the engine pointer has to
            // be destroyed rather than merely reset to give up its share.
            const auto owned = std::move(first);
        }
        EXPECT_EQ(finalized(), before) << "a surviving engine did not hold the plugin";
    }

    EXPECT_EQ(finalized(), before + 1);
}

} // namespace
