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

#include "plugin_loader_fixture_api.h"
#include "backend/backend_plugin.h"
#include "plugin_manager.h"
#include "../mocks/gmock_engine.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using loader_fixture::fixture_event_t;
using loader_fixture::fixture_mode_t;
using testing::_;
using testing::Ref;
using testing::Return;

namespace {

class proxyPluginTest : public testing::Test {
protected:
    void
    SetUp() override {
        // Match production: core exists before it loads any proxy globals.
        static_cast<void>(nixlPluginManager::getInstance());
        if (const char *value = std::getenv("NIXL_FAULT_CONFIG_FILE")) {
            originalConfig_ = value;
        }
        char directory[] = "/tmp/nixl-proxy-test-XXXXXX";
        const char *created = mkdtemp(directory);
        ASSERT_NE(created, nullptr);
        directory_ = created;
        configPath_ = directory_ / "config.toml";
        ASSERT_EQ(setenv("NIXL_FAULT_CONFIG_FILE", configPath_.c_str(), 1), 0);
        std::filesystem::create_symlink(PROXY_PLUGIN_PATH,
                                        directory_ / "libplugin_LOADER_FIXTURE.so");
        fixture_ = dlopen(LOADER_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(fixture_, nullptr) << dlerror();
        configure_ = reinterpret_cast<loader_fixture::configure_t>(
            dlsym(fixture_, "loader_fixture_configure"));
        ASSERT_NE(configure_, nullptr);
        configure();
        proxy_ =
            dlopen((directory_ / "libplugin_LOADER_FIXTURE.so").c_str(), RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(proxy_, nullptr) << dlerror();
        initialize_ = reinterpret_cast<nixlBackendPlugin *(*)()>(dlsym(proxy_, "nixl_plugin_init"));
        finalize_ = reinterpret_cast<void (*)()>(dlsym(proxy_, "nixl_plugin_fini"));
        ASSERT_NE(initialize_, nullptr);
        ASSERT_NE(finalize_, nullptr);
        writeConfig();
    }

    void
    TearDown() override {
        while (initializations_) {
            finalize();
        }
        if (proxy_) {
            dlclose(proxy_);
        }
        if (configure_) {
            configure_(fixture_mode_t::VALID, {});
        }
        if (fixture_) {
            dlclose(fixture_);
        }
        if (originalConfig_) {
            setenv("NIXL_FAULT_CONFIG_FILE", originalConfig_->c_str(), 1);
        } else {
            unsetenv("NIXL_FAULT_CONFIG_FILE");
        }
        if (!directory_.empty()) {
            std::filesystem::remove_all(directory_);
        }
    }

    void
    configure(fixture_mode_t mode = fixture_mode_t::VALID) {
        events_.clear();
        configure_(mode,
                   {this,
                    [](void *context, fixture_event_t event, const nixlBackendInitParams *params) {
                        auto &test = *static_cast<proxyPluginTest *>(context);
                        test.events_.push_back(event);
                        if (event == fixture_event_t::CREATE) {
                            test.createParams_ = params;
                        }
                    },
                    [](void *context, nixlBackendEngine *engine) {
                        static_cast<proxyPluginTest *>(context)->genuine_ =
                            dynamic_cast<mocks::GMockBackendEngine *>(engine);
                        auto &test = *static_cast<proxyPluginTest *>(context);
                        if (test.changeParams_) {
                            test.custom_["caller"] = "changed after genuine initialization";
                        }
                    }});
    }

    void
    writeConfig(const std::string &contents = "") {
        std::ofstream file(configPath_);
        ASSERT_TRUE(file);
        if (contents.empty()) {
            file << "[fault_injection]\nschema_version = 1\nexperiment_id = 'inert-test'\n"
                 << "real_plugin_path = '" << LOADER_FIXTURE_PATH << "'\nrules = []\n";
        } else {
            file << contents;
        }
    }

    nixlBackendPlugin *
    initialize() {
        auto *plugin = initialize_();
        if (plugin) {
            ++initializations_;
        }
        return plugin;
    }

    void
    finalize() {
        finalize_();
        --initializations_;
    }

    nixl_b_params_t custom_{{"caller", "value"}};
    nixlBackendInitParams params_{.localAgent = "local",
                                  .type = "LOADER_FIXTURE",
                                  .customParams = &custom_,
                                  .syncMode = nixl_thread_sync_t::NIXL_THREAD_SYNC_NONE};
    std::filesystem::path directory_;
    std::filesystem::path configPath_;
    std::vector<fixture_event_t> events_;
    const nixlBackendInitParams *createParams_ = nullptr;
    mocks::GMockBackendEngine *genuine_ = nullptr;
    bool changeParams_ = false;

private:
    std::optional<std::string> originalConfig_;
    void *fixture_ = nullptr;
    void *proxy_ = nullptr;
    loader_fixture::configure_t configure_ = nullptr;
    nixlBackendPlugin *(*initialize_)() = nullptr;
    void (*finalize_)() = nullptr;
    size_t initializations_ = 0;
};

TEST_F(proxyPluginTest, PreservesIdentityAndForwardsTransferWithOriginalHandles) {
    auto *plugin = initialize();
    ASSERT_NE(plugin, nullptr);
    EXPECT_STREQ(plugin->get_plugin_name(), "LOADER_FIXTURE");
    EXPECT_STREQ(plugin->get_plugin_version(), "1.2.3");
    EXPECT_EQ(plugin->get_backend_options(), (nixl_b_params_t{{"transport", "fixture"}}));
    EXPECT_EQ(plugin->get_backend_mems(), (nixl_mem_list_t{DRAM_SEG, VRAM_SEG}));
    auto engine = std::unique_ptr<nixlBackendEngine, void (*)(nixlBackendEngine *)>(
        plugin->create_engine(&params_), plugin->destroy_engine);
    ASSERT_NE(engine, nullptr);
    ASSERT_NE(genuine_, nullptr);
    EXPECT_EQ(createParams_, &params_);
    EXPECT_EQ(engine->getType(), params_.type);
    EXPECT_EQ(engine->getCustomParams(), custom_);
    nixl_meta_dlist_t local(DRAM_SEG), remote(DRAM_SEG);
    const std::string peer = "peer";
    // A genuine backend owns this handle; the wrapper must never replace it.
    nixlBackendReqH request;
    nixlBackendReqH *handle = nullptr;
    EXPECT_CALL(*genuine_, prepXfer(NIXL_WRITE, Ref(local), Ref(remote), Ref(peer), _, nullptr))
        .WillOnce(testing::DoAll(testing::SetArgReferee<4>(&request), Return(NIXL_SUCCESS)));
    EXPECT_CALL(*genuine_,
                postXfer(NIXL_WRITE, Ref(local), Ref(remote), Ref(peer), Ref(handle), nullptr))
        .WillOnce(Return(NIXL_IN_PROG));
    EXPECT_CALL(*genuine_, checkXfer(&request)).WillOnce(Return(NIXL_ERR_REMOTE_DISCONNECT));
    EXPECT_CALL(*genuine_, releaseReqH(&request)).WillOnce(Return(NIXL_SUCCESS));
    EXPECT_EQ(engine->prepXfer(NIXL_WRITE, local, remote, peer, handle), NIXL_SUCCESS);
    EXPECT_EQ(handle, &request);
    EXPECT_EQ(engine->postXfer(NIXL_WRITE, local, remote, peer, handle), NIXL_IN_PROG);
    EXPECT_EQ(engine->checkXfer(handle), NIXL_ERR_REMOTE_DISCONNECT);
    EXPECT_EQ(engine->releaseReqH(handle), NIXL_SUCCESS);
    finalize();
    EXPECT_EQ(events_,
              (std::vector<fixture_event_t>{fixture_event_t::INIT, fixture_event_t::CREATE}));
    engine.reset();
    EXPECT_EQ(events_,
              (std::vector<fixture_event_t>{fixture_event_t::INIT,
                                            fixture_event_t::CREATE,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED,
                                            fixture_event_t::FINALIZE}));
}

TEST_F(proxyPluginTest, BalancesRepeatedInitializationAndReadsConfigurationOnce) {
    auto *first = initialize();
    ASSERT_NE(first, nullptr);
    writeConfig("invalid TOML !");
    EXPECT_EQ(initialize(), first);
    EXPECT_EQ(events_, (std::vector<fixture_event_t>{fixture_event_t::INIT}));
    finalize();
    EXPECT_EQ(events_.size(), 1U);
    finalize();
    EXPECT_EQ(events_.back(), fixture_event_t::FINALIZE);
    EXPECT_EQ(initialize(), nullptr);
}

TEST_F(proxyPluginTest, RejectsMissingInvalidAndUnsupportedConfiguration) {
    unsetenv("NIXL_FAULT_CONFIG_FILE");
    EXPECT_EQ(initialize(), nullptr);
    setenv("NIXL_FAULT_CONFIG_FILE", configPath_.c_str(), 1);
    std::filesystem::remove(configPath_);
    EXPECT_EQ(initialize(), nullptr);
    const std::string base = "[fault_injection]\nschema_version = 1\nexperiment_id = 'test'\n";
    for (const auto &contents : std::vector<std::string>{
             "invalid TOML !",
             "[other]\n",
             "[fault_injection]\nschema_version = 2\n",
             "[fault_injection]\nschema_version = 1\n",
             base + "real_plugin_path = 'relative.so'\n",
             base + "real_plugin_path = '/absent/plugin.so'\n",
             base + "real_plugin_path = '" + PROXY_PLUGIN_PATH + "'\n",
             base + "real_plugin_path = '" + LOADER_FIXTURE_PATH + "'\nunknown = 1\n",
             base + "real_plugin_path = '" + LOADER_FIXTURE_PATH + "'\nrules = [{}]\n",
             base + "real_plugin_path = '" + LOADER_FIXTURE_PATH + "'\nrules = 'invalid'\n"}) {
        SCOPED_TRACE(contents);
        writeConfig(contents);
        EXPECT_EQ(initialize(), nullptr);
    }
    EXPECT_TRUE(events_.empty());
    writeConfig();
    EXPECT_NE(initialize(), nullptr);
}

TEST_F(proxyPluginTest, ContainsFactoryFailuresAndCleansUpWrapperConstructionFailure) {
    auto *plugin = initialize();
    ASSERT_NE(plugin, nullptr);
    EXPECT_EQ(plugin->create_engine(nullptr), nullptr);
    configure(fixture_mode_t::NULL_ENGINE);
    EXPECT_EQ(plugin->create_engine(&params_), nullptr);
    configure(fixture_mode_t::THROW_CREATE);
    EXPECT_EQ(plugin->create_engine(&params_), nullptr);
    configure();
    changeParams_ = true;
    EXPECT_EQ(plugin->create_engine(&params_), nullptr);
    EXPECT_EQ(events_,
              (std::vector<fixture_event_t>{fixture_event_t::CREATE,
                                            fixture_event_t::DESTROY_CALLBACK,
                                            fixture_event_t::ENGINE_DESTROYED}));
}

// Run in its own process: the core plugin manager retains its loaded plugin until process exit.
TEST_F(proxyPluginTest, CoreDiscovery) {
    auto &manager = nixlPluginManager::getInstance();
    manager.addPluginDirectory(directory_.string());
    auto plugin = manager.loadBackendPlugin("LOADER_FIXTURE");
    ASSERT_NE(plugin, nullptr);
    EXPECT_STREQ(plugin->getName(), "LOADER_FIXTURE");
    auto engine = std::unique_ptr<nixlBackendEngine, void (*)(nixlBackendEngine *)>(
        plugin->createEngine(&params_), [](nixlBackendEngine *value) { delete value; });
    ASSERT_NE(engine, nullptr);
    ASSERT_NE(genuine_, nullptr);
    EXPECT_CALL(*genuine_, connect("peer")).WillOnce(Return(NIXL_ERR_BACKEND));
    EXPECT_EQ(engine->connect("peer"), NIXL_ERR_BACKEND);
}

} // namespace
