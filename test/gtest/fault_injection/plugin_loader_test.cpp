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

// Verify plugin validation, metadata, engine creation, and lifetime/cleanup ordering.

#include "forwarding_engine.h"
#include "plugin_loader.h"
#include "plugin_loader_fixture_api.h"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

using nixl::fault::engine_ptr_t;
using nixl::fault::forwardingEngine;
using nixl::fault::pluginLoader;
static_assert(!std::is_copy_constructible_v<pluginLoader>);
static_assert(!std::is_copy_assignable_v<pluginLoader>);
static_assert(!std::is_move_constructible_v<pluginLoader>);
static_assert(!std::is_move_assignable_v<pluginLoader>);
using loader_fixture::fixture_mode_t;
using loader_fixture::fixture_event_t;

namespace {
thread_local bool suppress_next_dlerror = false;
}

// The test's --wrap=dlerror option lets one case suppress a loader diagnostic.
extern "C" char *
__real_dlerror() noexcept;

extern "C" char *
__wrap_dlerror() noexcept {
    char *error = __real_dlerror();
    return std::exchange(suppress_next_dlerror, false) ? nullptr : error;
}

namespace {

// Observe logical init/fini and engine cleanup without relying on physical dlclose
// unloading: core's RTLD_NODELETE workaround deliberately keeps code resident.
class fixturePlugin {
public:
    explicit fixturePlugin(const char *path = LOADER_FIXTURE_PATH) {
        handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            throw std::runtime_error(dlerror());
        }
        configure_ = reinterpret_cast<loader_fixture::configure_t>(
            dlsym(handle_, "loader_fixture_configure"));
        if (!configure_) {
            dlclose(handle_);
            throw std::runtime_error("Missing fixture configuration function");
        }
        configure();
    }

    ~fixturePlugin() {
        configure_(fixture_mode_t::VALID, {});
        dlclose(handle_);
    }

    fixturePlugin(const fixturePlugin &) = delete;
    fixturePlugin &
    operator=(const fixturePlugin &) = delete;

    void
    configure(fixture_mode_t mode = fixture_mode_t::VALID) {
        events.clear();
        createParams.clear();
        configure_(
            mode,
            {this, [](void *context, fixture_event_t event, const nixlBackendInitParams *params) {
                 auto &fixture = *static_cast<fixturePlugin *>(context);
                 fixture.events.push_back(event);
                 if (event == fixture_event_t::CREATE) {
                     fixture.createParams.push_back(params);
                 }
             }});
    }

    bool
    rejectsSelfLoad(const std::filesystem::path &path) const {
        using probe_t = bool (*)(const char *);
        const auto probe = reinterpret_cast<probe_t>(dlsym(handle_, "loader_fixture_rejects_self"));
        if (!probe) {
            throw std::runtime_error("Missing fixture self-load probe");
        }
        return probe(path.c_str());
    }

    std::vector<fixture_event_t> events;
    std::vector<const nixlBackendInitParams *> createParams;

private:
    void *handle_;
    loader_fixture::configure_t configure_;
};

class PluginLoaderTest : public testing::Test {
protected:
    nixl_b_params_t custom{{"caller", "value"}};
    nixlBackendInitParams params{.localAgent = "local",
                                 .type = "LOADER_FIXTURE",
                                 .customParams = &custom,
                                 .enableProgTh = true,
                                 .pthrDelay = 123,
                                 .syncMode = nixl_thread_sync_t::NIXL_THREAD_SYNC_NONE,
                                 .enableTelemetry_ = true};
};

class selfLoadTest : public testing::Test {
protected:
    void
    SetUp() override {
        fixturePath = std::filesystem::temp_directory_path() /
            ("nixl_loader_alias_" + std::to_string(getpid()) + ".so");
        aliasPath = fixturePath.string() + ".alias";
        std::filesystem::copy_file(LOADER_FIXTURE_PATH, fixturePath);
        std::filesystem::create_symlink(fixturePath, aliasPath);
    }

    void
    TearDown() override {
        std::error_code ec;
        std::filesystem::remove(aliasPath, ec);
        std::filesystem::remove(fixturePath, ec);
    }

    std::filesystem::path fixturePath;
    std::filesystem::path aliasPath;
};

void
expectLoadFailure(const std::filesystem::path &path, const char *message) {
    try {
        pluginLoader loader(path);
        FAIL() << "Expected plugin loading to fail";
    }
    catch (const std::runtime_error &error) {
        const std::string diagnostic = error.what();
        EXPECT_NE(diagnostic.find(path.string()), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find(message), std::string::npos) << diagnostic;
    }
}

TEST_F(PluginLoaderTest, ReportsPluginMetadataAndFinalizesOnce) {
    fixturePlugin fixture;
    {
        pluginLoader loader(LOADER_FIXTURE_PATH);
        const char *name = loader.getName();
        const char *version = loader.getVersion();
        EXPECT_STREQ(name, "LOADER_FIXTURE");
        EXPECT_STREQ(version, "1.2.3");
        EXPECT_EQ(loader.getBackendOptions(), (nixl_b_params_t{{"transport", "fixture"}}));
        EXPECT_EQ(loader.getBackendMems(), (nixl_mem_list_t{DRAM_SEG, VRAM_SEG}));
        EXPECT_EQ(fixture.events, (std::vector<fixture_event_t>{fixture_event_t::INIT}));
    }
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{fixture_event_t::INIT, fixture_event_t::FINALIZE}));
}

TEST_F(PluginLoaderTest, PassesOriginalParametersAndUsesPluginDestroyCallback) {
    fixturePlugin fixture;
    {
        pluginLoader loader(LOADER_FIXTURE_PATH);
        {
            auto engine = loader.createEngine(&params);
            ASSERT_EQ(fixture.createParams.size(), 1U);
            EXPECT_EQ(fixture.createParams.front(), &params);
            EXPECT_EQ(engine->getType(), params.type);
            EXPECT_EQ(engine->getCustomParams(), custom);
            EXPECT_FALSE(engine->getInitErr());
        }
        EXPECT_EQ(fixture.events,
                  (std::vector<fixture_event_t>{fixture_event_t::INIT,
                                                fixture_event_t::CREATE,
                                                fixture_event_t::DESTROY_CALLBACK,
                                                fixture_event_t::ENGINE_DESTROYED}));
    }
    EXPECT_EQ(fixture.events.back(), fixture_event_t::FINALIZE);
    EXPECT_EQ(fixture.events.size(), 5U);
}

TEST_F(PluginLoaderTest, DefersFinalizationUntilEngineAndForwarderAreDestroyed) {
    fixturePlugin fixture;
    std::optional<engine_ptr_t> first;
    std::optional<forwardingEngine> second;
    {
        pluginLoader loader(LOADER_FIXTURE_PATH);
        first.emplace(loader.createEngine(&params));
        second.emplace(&params, loader.createEngine(&params));
    }
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{
                  fixture_event_t::INIT, fixture_event_t::CREATE, fixture_event_t::CREATE}));
    first.reset();
    EXPECT_TRUE(second->supportsLocal());
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{fixture_event_t::INIT,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED}));
    second.reset();
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{fixture_event_t::INIT,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED,
                                            fixture_event_t::FINALIZE}));
}

TEST_F(PluginLoaderTest, AcceptsNullParameterMap) {
    fixturePlugin fixture;
    pluginLoader loader(LOADER_FIXTURE_PATH);
    params.customParams = nullptr;
    auto engine = loader.createEngine(&params);
    EXPECT_TRUE(engine->getCustomParams().empty());
    EXPECT_EQ(fixture.createParams.front(), &params);
}

TEST_F(PluginLoaderTest, PreservesEngineInitializationErrorForCaller) {
    fixturePlugin fixture;
    fixture.configure(fixture_mode_t::ENGINE_INIT_ERROR);
    pluginLoader loader(LOADER_FIXTURE_PATH);
    auto engine = loader.createEngine(&params);
    EXPECT_TRUE(engine->getInitErr());
}

TEST_F(PluginLoaderTest, RejectsNullParametersBeforeCallingFactory) {
    fixturePlugin fixture;
    pluginLoader loader(LOADER_FIXTURE_PATH);
    EXPECT_THROW(loader.createEngine(nullptr), std::invalid_argument);
    EXPECT_EQ(fixture.events, (std::vector<fixture_event_t>{fixture_event_t::INIT}));
}

TEST_F(PluginLoaderTest, RejectsRelativeAndEmptyPaths) {
    EXPECT_THROW(pluginLoader("relative.so"), std::invalid_argument);
    EXPECT_THROW(pluginLoader(""), std::invalid_argument);
}

TEST_F(PluginLoaderTest, RejectsSelfLoadAndSymlink) {
    Dl_info info{};
    ASSERT_NE(dladdr(reinterpret_cast<void *>(&expectLoadFailure), &info), 0);
    ASSERT_NE(info.dli_fname, nullptr);
    const auto self_path = std::filesystem::absolute(info.dli_fname);
    EXPECT_THROW(pluginLoader{self_path}, std::invalid_argument);

    const auto link = std::filesystem::temp_directory_path() /
        ("nixl_loader_self_" + std::to_string(getpid()) + ".so");
    std::filesystem::create_symlink(self_path, link);
    EXPECT_THROW(pluginLoader{link}, std::invalid_argument);
    EXPECT_TRUE(std::filesystem::remove(link));
}

TEST_F(selfLoadTest, RejectsSelfLoadAfterAliasIsRemoved) {
    fixturePlugin fixture(aliasPath.c_str());
    ASSERT_TRUE(std::filesystem::remove(aliasPath));
    EXPECT_TRUE(fixture.rejectsSelfLoad(fixturePath));
    EXPECT_FALSE(fixture.rejectsSelfLoad(LOADER_FIXTURE_PATH));
    EXPECT_TRUE(fixture.events.empty());
}

TEST_F(PluginLoaderTest, AcceptsDeepBindingRequest) {
    if (LOADER_TEST_SANITIZED) {
        GTEST_SKIP() << "RTLD_DEEPBIND bypasses sanitizer interposition";
    }
    pluginLoader loader(LOADER_FIXTURE_PATH, true);
    EXPECT_STREQ(loader.getName(), "LOADER_FIXTURE");
}

TEST_F(PluginLoaderTest, ReportsMissingFile) {
    expectLoadFailure(std::string(LOADER_FIXTURE_PATH) + ".missing", "Cannot load genuine plugin");
}

TEST_F(PluginLoaderTest, ReportsLoadFailureWithoutDiagnostic) {
    suppress_next_dlerror = true;
    expectLoadFailure(std::string(LOADER_FIXTURE_PATH) + ".missing", "unknown error");
}

TEST_F(PluginLoaderTest, AcceptsPluginWithoutFinalizer) {
    fixturePlugin fixture(LOADER_NO_FINI_PATH);
    {
        pluginLoader loader(LOADER_NO_FINI_PATH);
        EXPECT_EQ(dlerror(), nullptr);
        auto engine = loader.createEngine(&params);
        EXPECT_STREQ(loader.getName(), "LOADER_FIXTURE");
    }
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{fixture_event_t::INIT,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED}));
}

struct invalidPlugin {
    fixture_mode_t mode;
    const char *name;
    const char *message;
    bool finalize;
};

// Each case corrupts one part of the exported plugin contract. Successful init
// must be paired with fini even when validation rejects the returned interface.
class InvalidPluginTest : public testing::TestWithParam<invalidPlugin> {};

TEST_P(InvalidPluginTest, ReportsFailureAndCleansUp) {
    const auto &scenario = GetParam();
    fixturePlugin fixture;
    fixture.configure(scenario.mode);
    expectLoadFailure(LOADER_FIXTURE_PATH, scenario.message);
    const auto expected = scenario.finalize ?
        std::vector<fixture_event_t>{fixture_event_t::INIT, fixture_event_t::FINALIZE} :
        std::vector<fixture_event_t>{fixture_event_t::INIT};
    EXPECT_EQ(fixture.events, expected);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidInterfaces,
    InvalidPluginTest,
    testing::Values(
        invalidPlugin{fixture_mode_t::NULL_INIT, "NullInit", "initialization failed", false},
        invalidPlugin{fixture_mode_t::THROW_INIT, "ThrowInit", "fixture init failed", false},
        invalidPlugin{fixture_mode_t::OLD_API, "OldApi", "API version mismatch", true},
        invalidPlugin{fixture_mode_t::NO_CREATE, "NoCreate", "required callbacks", true},
        invalidPlugin{fixture_mode_t::NO_DESTROY, "NoDestroy", "required callbacks", true},
        invalidPlugin{fixture_mode_t::NO_NAME_CALLBACK,
                      "NoNameCallback",
                      "required callbacks",
                      true},
        invalidPlugin{fixture_mode_t::NO_VERSION_CALLBACK,
                      "NoVersionCallback",
                      "required callbacks",
                      true},
        invalidPlugin{fixture_mode_t::NULL_NAME, "NullName", "empty name or version", true},
        invalidPlugin{fixture_mode_t::EMPTY_NAME, "EmptyName", "empty name or version", true},
        invalidPlugin{fixture_mode_t::NULL_VERSION, "NullVersion", "empty name or version", true},
        invalidPlugin{fixture_mode_t::EMPTY_VERSION,
                      "EmptyVersion",
                      "empty name or version",
                      true}),
    [](const testing::TestParamInfo<invalidPlugin> &info) { return info.param.name; });

class OptionalCallbacksTest : public PluginLoaderTest,
                              public testing::WithParamInterface<fixture_mode_t> {};

TEST_P(OptionalCallbacksTest, DefaultsAbsentCallbacksToEmptyResults) {
    fixturePlugin fixture;
    fixture.configure(GetParam());
    pluginLoader loader(LOADER_FIXTURE_PATH);
    const auto options = GetParam() == fixture_mode_t::NO_MEMS ?
        nixl_b_params_t{{"transport", "fixture"}} :
        nixl_b_params_t{};
    const auto mems = GetParam() == fixture_mode_t::NO_OPTIONS ?
        nixl_mem_list_t{DRAM_SEG, VRAM_SEG} :
        nixl_mem_list_t{};
    EXPECT_EQ(loader.getBackendOptions(), options);
    EXPECT_EQ(loader.getBackendMems(), mems);
}

INSTANTIATE_TEST_SUITE_P(MissingOptionalCallbacks,
                         OptionalCallbacksTest,
                         testing::Values(fixture_mode_t::NO_OPTIONS,
                                         fixture_mode_t::NO_MEMS,
                                         fixture_mode_t::NO_OPTIONAL));

class FailedEngineTest : public PluginLoaderTest,
                         public testing::WithParamInterface<fixture_mode_t> {};

TEST_P(FailedEngineTest, FailedFactoryLeavesPluginUsableAndFinalizesOnce) {
    fixturePlugin fixture;
    fixture.configure(GetParam());
    {
        pluginLoader loader(LOADER_FIXTURE_PATH);
        EXPECT_THROW(loader.createEngine(&params), std::runtime_error);
        EXPECT_STREQ(loader.getName(), "LOADER_FIXTURE");
        EXPECT_EQ(fixture.events,
                  (std::vector<fixture_event_t>{fixture_event_t::INIT, fixture_event_t::CREATE}));
    }
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{
                  fixture_event_t::INIT, fixture_event_t::CREATE, fixture_event_t::FINALIZE}));
}

INSTANTIATE_TEST_SUITE_P(FailedFactories,
                         FailedEngineTest,
                         testing::Values(fixture_mode_t::NULL_ENGINE,
                                         fixture_mode_t::THROW_CREATE));

class cleanupFailureTest : public PluginLoaderTest,
                           public testing::WithParamInterface<fixture_mode_t> {};

TEST_P(cleanupFailureTest, LogsCleanupFailuresAndPreservesCallerException) {
    fixturePlugin fixture;
    fixture.configure(GetParam());
    testing::internal::CaptureStderr();
    EXPECT_THROW(
        {
            pluginLoader loader(LOADER_FIXTURE_PATH);
            auto engine = loader.createEngine(&params);
            throw std::logic_error("caller failure");
        },
        std::logic_error);
    const std::string diagnostic = testing::internal::GetCapturedStderr();
    const std::string destroy_context =
        "engine destruction threw in '" + std::string(LOADER_FIXTURE_PATH) + "'";
    EXPECT_NE(diagnostic.find(destroy_context), std::string::npos) << diagnostic;
    EXPECT_NE(diagnostic.find("finalization threw"), std::string::npos) << diagnostic;
    if (GetParam() == fixture_mode_t::THROW_CLEANUP) {
        EXPECT_NE(diagnostic.find("fixture destroy failed"), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find("fixture finalization failed"), std::string::npos) << diagnostic;
    } else {
        EXPECT_NE(diagnostic.find("unknown exception"), std::string::npos) << diagnostic;
    }
    EXPECT_EQ(fixture.events,
              (std::vector<fixture_event_t>{fixture_event_t::INIT,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED,
                                            fixture_event_t::FINALIZE}));
}

INSTANTIATE_TEST_SUITE_P(CleanupExceptions,
                         cleanupFailureTest,
                         testing::Values(fixture_mode_t::THROW_CLEANUP,
                                         fixture_mode_t::THROW_UNKNOWN_CLEANUP),
                         [](const testing::TestParamInfo<fixture_mode_t> &info) {
                             return info.param == fixture_mode_t::THROW_CLEANUP ? "Standard" :
                                                                                  "Unknown";
                         });

} // namespace
