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

#include "plugin_loader.h"
#include "plugin_loader_fixture_api.h"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

// Competes with the plugin's own definition to show whether it was deep-bound.
extern "C" __attribute__((visibility("default"))) int
loader_deepbind_value() {
    return 2;
}

using nixl::fault::PluginLoader;
static_assert(!std::is_copy_constructible_v<PluginLoader>);
static_assert(!std::is_copy_assignable_v<PluginLoader>);
using loader_fixture::Mode;
using loader_fixture::Event;

namespace {

// Observe logical init/fini and engine cleanup without relying on physical dlclose
// unloading: core's RTLD_NODELETE workaround deliberately keeps code resident.
class Fixture {
public:
    explicit Fixture(const char *path = LOADER_FIXTURE_PATH) {
        handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            throw std::runtime_error(dlerror());
        }
        configure_ =
            reinterpret_cast<loader_fixture::Configure>(dlsym(handle_, "loader_fixture_configure"));
        if (!configure_) {
            dlclose(handle_);
            throw std::runtime_error("Missing fixture configuration function");
        }
        configure();
    }

    ~Fixture() {
        configure_(Mode::Valid, {});
        dlclose(handle_);
    }

    Fixture(const Fixture &) = delete;
    Fixture &
    operator=(const Fixture &) = delete;

    void
    configure(Mode mode = Mode::Valid) {
        events.clear();
        createParams.clear();
        configure_(mode,
                   {this, [](void *context, Event event, const nixlBackendInitParams *params) {
                        auto &fixture = *static_cast<Fixture *>(context);
                        fixture.events.push_back(event);
                        if (event == Event::Create) {
                            fixture.createParams.push_back(params);
                        }
                    }});
    }

    std::vector<Event> events;
    std::vector<const nixlBackendInitParams *> createParams;

private:
    void *handle_;
    loader_fixture::Configure configure_;
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

void
expectLoadFailure(const std::filesystem::path &path, const char *message) {
    try {
        PluginLoader loader(path);
        FAIL() << "Expected plugin loading to fail";
    }
    catch (const std::runtime_error &error) {
        const std::string diagnostic = error.what();
        EXPECT_NE(diagnostic.find(path.string()), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find(message), std::string::npos) << diagnostic;
    }
}

TEST_F(PluginLoaderTest, ReportsPluginMetadataAndFinalizesOnce) {
    Fixture fixture;
    {
        PluginLoader loader(LOADER_FIXTURE_PATH);
        EXPECT_EQ(loader.getName(), "LOADER_FIXTURE");
        EXPECT_EQ(loader.getVersion(), "1.2.3");
        EXPECT_EQ(loader.getBackendOptions(), (nixl_b_params_t{{"transport", "fixture"}}));
        EXPECT_EQ(loader.getBackendMems(), (nixl_mem_list_t{DRAM_SEG, VRAM_SEG}));
        EXPECT_EQ(fixture.events, (std::vector<Event>{Event::Init}));
    }
    EXPECT_EQ(fixture.events, (std::vector<Event>{Event::Init, Event::Finalize}));
}

TEST_F(PluginLoaderTest, PassesOriginalParametersAndUsesPluginDestroyCallback) {
    Fixture fixture;
    {
        PluginLoader loader(LOADER_FIXTURE_PATH);
        {
            auto engine = loader.createEngine(&params);
            ASSERT_EQ(fixture.createParams.size(), 1U);
            EXPECT_EQ(fixture.createParams.front(), &params);
            EXPECT_EQ(engine->getType(), params.type);
            EXPECT_EQ(engine->getCustomParams(), custom);
            EXPECT_FALSE(engine->getInitErr());
        }
        EXPECT_EQ(fixture.events,
                  (std::vector<Event>{
                      Event::Init, Event::Create, Event::DestroyCallback, Event::EngineDestroyed}));
    }
    EXPECT_EQ(fixture.events.back(), Event::Finalize);
    EXPECT_EQ(fixture.events.size(), 5U);
}

TEST_F(PluginLoaderTest, DefersFinalizationUntilLastEngineOwnerIsDestroyed) {
    Fixture fixture;
    std::optional<PluginLoader::EnginePtr> first;
    std::optional<PluginLoader::EnginePtr> second;
    {
        PluginLoader loader(LOADER_FIXTURE_PATH);
        first.emplace(loader.createEngine(&params));
        second.emplace(loader.createEngine(&params));
    }
    EXPECT_EQ(fixture.events, (std::vector<Event>{Event::Init, Event::Create, Event::Create}));
    first.reset();
    EXPECT_TRUE((*second)->supportsLocal());
    EXPECT_EQ(fixture.events,
              (std::vector<Event>{Event::Init,
                                  Event::Create,
                                  Event::Create,
                                  Event::DestroyCallback,
                                  Event::EngineDestroyed}));
    second.reset();
    EXPECT_EQ(fixture.events,
              (std::vector<Event>{Event::Init,
                                  Event::Create,
                                  Event::Create,
                                  Event::DestroyCallback,
                                  Event::EngineDestroyed,
                                  Event::DestroyCallback,
                                  Event::EngineDestroyed,
                                  Event::Finalize}));
}

TEST_F(PluginLoaderTest, MovingEngineOwnershipDoesNotDestroyEngineTwice) {
    Fixture fixture;
    {
        PluginLoader loader(LOADER_FIXTURE_PATH);
        auto engine = loader.createEngine(&params);
        auto moved = std::move(engine);
        EXPECT_FALSE(engine);
        EXPECT_TRUE(moved->supportsLocal());
    }
    EXPECT_EQ(fixture.events,
              (std::vector<Event>{Event::Init,
                                  Event::Create,
                                  Event::DestroyCallback,
                                  Event::EngineDestroyed,
                                  Event::Finalize}));
}

TEST_F(PluginLoaderTest, ResetEngineRetainsLibraryThroughItsDeleter) {
    Fixture fixture;
    std::optional<PluginLoader::EnginePtr> owner;
    {
        PluginLoader loader(LOADER_FIXTURE_PATH);
        owner.emplace(loader.createEngine(&params));
    }
    (*owner).reset();
    EXPECT_EQ(fixture.events,
              (std::vector<Event>{
                  Event::Init, Event::Create, Event::DestroyCallback, Event::EngineDestroyed}));
    owner.reset();
    EXPECT_EQ(fixture.events.back(), Event::Finalize);
    EXPECT_EQ(fixture.events.size(), 5U);
}

TEST_F(PluginLoaderTest, AcceptsNullParameterMap) {
    Fixture fixture;
    PluginLoader loader(LOADER_FIXTURE_PATH);
    params.customParams = nullptr;
    auto engine = loader.createEngine(&params);
    EXPECT_TRUE(engine->getCustomParams().empty());
    EXPECT_EQ(fixture.createParams.front(), &params);
}

TEST_F(PluginLoaderTest, PreservesEngineInitializationErrorForCaller) {
    Fixture fixture;
    fixture.configure(Mode::EngineInitError);
    PluginLoader loader(LOADER_FIXTURE_PATH);
    auto engine = loader.createEngine(&params);
    EXPECT_TRUE(engine->getInitErr());
}

TEST_F(PluginLoaderTest, RejectsNullParametersBeforeCallingFactory) {
    Fixture fixture;
    PluginLoader loader(LOADER_FIXTURE_PATH);
    EXPECT_THROW(loader.createEngine(nullptr), std::invalid_argument);
    EXPECT_EQ(fixture.events, (std::vector<Event>{Event::Init}));
}

TEST_F(PluginLoaderTest, RejectsRelativeAndEmptyPaths) {
    EXPECT_THROW(PluginLoader("relative.so"), std::invalid_argument);
    EXPECT_THROW(PluginLoader(""), std::invalid_argument);
}

TEST_F(PluginLoaderTest, RejectsLoadingTheProxyItself) {
    void *handle = dlopen(LOADER_SELF_LOAD_PATH, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(handle, nullptr) << dlerror();
    using Probe = const char *(*)(const char *);
    const auto probe = reinterpret_cast<Probe>(dlsym(handle, "loader_self_load_probe"));
    ASSERT_NE(probe, nullptr);
    const std::string diagnostic = probe(LOADER_SELF_LOAD_PATH);
    EXPECT_NE(diagnostic.find("refers to the proxy itself"), std::string::npos) << diagnostic;
    // A symlink to the same file is still the same object.
    const auto link = std::filesystem::path(testing::TempDir()) / "self_load_link.so";
    std::filesystem::remove(link);
    std::filesystem::create_symlink(LOADER_SELF_LOAD_PATH, link);
    const std::string linked = probe(link.c_str());
    EXPECT_NE(linked.find("refers to the proxy itself"), std::string::npos) << linked;
    std::filesystem::remove(link);
    dlclose(handle);
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LOADER_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LOADER_TEST_SANITIZED 1
#endif
#endif

#if !defined(LOADER_TEST_SANITIZED) && defined(RTLD_DEEPBIND)
// Binding is fixed when a library is first loaded, so each mode gets its own copy
// of the fixture that only the loader ever opens.
int
probeDeepBind(const char *path, bool deepbind) {
    PluginLoader loader(path, deepbind);
    void *handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    EXPECT_NE(handle, nullptr);
    using Probe = int (*)();
    const auto probe = reinterpret_cast<Probe>(dlsym(handle, "loader_fixture_deepbind_probe"));
    EXPECT_NE(probe, nullptr);
    const int value = probe();
    dlclose(handle);
    return value;
}
#endif

TEST_F(PluginLoaderTest, DeepBindIsOptIn) {
#if defined(LOADER_TEST_SANITIZED) || !defined(RTLD_DEEPBIND)
    GTEST_SKIP() << "RTLD_DEEPBIND is unavailable or incompatible with sanitizer runtimes";
#else
    // Default: the executable's symbol wins. Deep bound: the plugin's own dependency wins.
    EXPECT_EQ(probeDeepBind(LOADER_PLAIN_BIND_PATH, false), 2);
    EXPECT_EQ(probeDeepBind(LOADER_DEEPBIND_PATH, true), 1);
#endif
}

TEST_F(PluginLoaderTest, ReportsMissingFile) {
    expectLoadFailure(std::string(LOADER_FIXTURE_PATH) + ".missing", "Cannot load genuine plugin");
}

TEST_F(PluginLoaderTest, ReportsMissingInitializationSymbol) {
    expectLoadFailure(LOADER_MISSING_INIT_PATH, "nixl_plugin_init");
}

TEST_F(PluginLoaderTest, AcceptsPluginWithoutFinalizer) {
    Fixture fixture(LOADER_NO_FINI_PATH);
    {
        PluginLoader loader(LOADER_NO_FINI_PATH);
        auto engine = loader.createEngine(&params);
        EXPECT_EQ(loader.getName(), "LOADER_FIXTURE");
    }
    EXPECT_EQ(fixture.events,
              (std::vector<Event>{
                  Event::Init, Event::Create, Event::DestroyCallback, Event::EngineDestroyed}));
}

struct InvalidPlugin {
    Mode mode;
    const char *name;
    const char *message;
    bool finalize;
};

// Each case corrupts one part of the exported plugin contract. Successful init
// must be paired with fini even when validation rejects the returned interface.
class InvalidPluginTest : public testing::TestWithParam<InvalidPlugin> {};

TEST_P(InvalidPluginTest, ReportsFailureAndCleansUp) {
    const auto &scenario = GetParam();
    Fixture fixture;
    fixture.configure(scenario.mode);
    expectLoadFailure(LOADER_FIXTURE_PATH, scenario.message);
    const auto expected = scenario.finalize ? std::vector<Event>{Event::Init, Event::Finalize} :
                                              std::vector<Event>{Event::Init};
    EXPECT_EQ(fixture.events, expected);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidInterfaces,
    InvalidPluginTest,
    testing::Values(
        InvalidPlugin{Mode::NullInit, "NullInit", "initialization failed", false},
        InvalidPlugin{Mode::ThrowInit, "ThrowInit", "fixture init failed", false},
        InvalidPlugin{Mode::OldApi, "OldApi", "API version mismatch", true},
        InvalidPlugin{Mode::NoCreate, "NoCreate", "required callbacks", true},
        InvalidPlugin{Mode::NoDestroy, "NoDestroy", "required callbacks", true},
        InvalidPlugin{Mode::NoNameCallback, "NoNameCallback", "required callbacks", true},
        InvalidPlugin{Mode::NoVersionCallback, "NoVersionCallback", "required callbacks", true},
        InvalidPlugin{Mode::NullName, "NullName", "empty name or version", true},
        InvalidPlugin{Mode::EmptyName, "EmptyName", "empty name or version", true},
        InvalidPlugin{Mode::NullVersion, "NullVersion", "empty name or version", true},
        InvalidPlugin{Mode::EmptyVersion, "EmptyVersion", "empty name or version", true}),
    [](const testing::TestParamInfo<InvalidPlugin> &info) { return info.param.name; });

class OptionalCallbacksTest : public PluginLoaderTest, public testing::WithParamInterface<Mode> {};

TEST_P(OptionalCallbacksTest, DefaultsAbsentCallbacksToEmptyResults) {
    Fixture fixture;
    fixture.configure(GetParam());
    PluginLoader loader(LOADER_FIXTURE_PATH);
    const auto options =
        GetParam() == Mode::NoMems ? nixl_b_params_t{{"transport", "fixture"}} : nixl_b_params_t{};
    const auto mems =
        GetParam() == Mode::NoOptions ? nixl_mem_list_t{DRAM_SEG, VRAM_SEG} : nixl_mem_list_t{};
    EXPECT_EQ(loader.getBackendOptions(), options);
    EXPECT_EQ(loader.getBackendMems(), mems);
}

INSTANTIATE_TEST_SUITE_P(MissingOptionalCallbacks,
                         OptionalCallbacksTest,
                         testing::Values(Mode::NoOptions, Mode::NoMems, Mode::NoOptional));

class FailedEngineTest : public PluginLoaderTest, public testing::WithParamInterface<Mode> {};

TEST_P(FailedEngineTest, FailedFactoryLeavesPluginUsableAndFinalizesOnce) {
    Fixture fixture;
    fixture.configure(GetParam());
    {
        PluginLoader loader(LOADER_FIXTURE_PATH);
        EXPECT_THROW(loader.createEngine(&params), std::runtime_error);
        EXPECT_EQ(loader.getName(), "LOADER_FIXTURE");
        EXPECT_EQ(fixture.events, (std::vector<Event>{Event::Init, Event::Create}));
    }
    EXPECT_EQ(fixture.events, (std::vector<Event>{Event::Init, Event::Create, Event::Finalize}));
}

INSTANTIATE_TEST_SUITE_P(FailedFactories,
                         FailedEngineTest,
                         testing::Values(Mode::NullEngine, Mode::ThrowCreate));

} // namespace
