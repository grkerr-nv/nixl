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

// Fake plugin for unit tests to configure behavior and observe lifecycle events.

#include "plugin_loader_fixture_api.h"
#include "backend/backend_plugin.h"
#include "../mocks/gmock_engine.h"

#include <stdexcept>

namespace {
using loader_fixture::fixture_mode_t;
using loader_fixture::fixture_event_t;
fixture_mode_t mode = fixture_mode_t::VALID;
loader_fixture::observer observer;

void
record(fixture_event_t event, const nixlBackendInitParams *params = nullptr) {
    if (observer.record) {
        observer.record(observer.context, event, params);
    }
}

class fixtureEngine : public mocks::GMockBackendEngine {
public:
    explicit fixtureEngine(const nixlBackendInitParams *params)
        : mocks::GMockBackendEngine(params) {
        initErr = mode == fixture_mode_t::ENGINE_INIT_ERROR;
    }

    ~fixtureEngine() override {
        record(fixture_event_t::ENGINE_DESTROYED);
    }
};

nixlBackendEngine *
create(const nixlBackendInitParams *params) {
    record(fixture_event_t::CREATE, params);
    if (mode == fixture_mode_t::NULL_ENGINE) {
        return nullptr;
    }
    if (mode == fixture_mode_t::THROW_CREATE) {
        throw std::runtime_error("fixture create failed");
    }
    auto *engine = new testing::NiceMock<fixtureEngine>(params);
    if (observer.engineCreated) {
        observer.engineCreated(observer.context, engine);
    }
    return engine;
}

void
destroy(nixlBackendEngine *engine) {
    record(fixture_event_t::DESTROY_CALLBACK);
    delete engine;
}

const char *
name() {
    if (mode == fixture_mode_t::NULL_NAME) {
        return nullptr;
    }
    return mode == fixture_mode_t::EMPTY_NAME ? "" : "LOADER_FIXTURE";
}

const char *
version() {
    if (mode == fixture_mode_t::NULL_VERSION) {
        return nullptr;
    }
    return mode == fixture_mode_t::EMPTY_VERSION ? "" : "1.2.3";
}

nixl_b_params_t
options() {
    return {{"transport", "fixture"}};
}

nixl_mem_list_t
mems() {
    return {DRAM_SEG, VRAM_SEG};
}

} // namespace

extern "C" NIXL_PLUGIN_EXPORT void
loader_fixture_configure(fixture_mode_t next, loader_fixture::observer next_observer) {
    mode = next;
    observer = next_observer;
}

extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    record(fixture_event_t::INIT);
    if (mode == fixture_mode_t::NULL_INIT) {
        return nullptr;
    }
    if (mode == fixture_mode_t::THROW_INIT) {
        throw std::runtime_error("fixture init failed");
    }
    static nixlBackendPlugin plugin;
    plugin = {NIXL_PLUGIN_API_VERSION, create, destroy, name, version, options, mems};
    switch (mode) {
    case fixture_mode_t::OLD_API:
        plugin.api_version = NIXL_PLUGIN_API_VERSION - 1;
        break;
    case fixture_mode_t::NO_CREATE:
        plugin.create_engine = nullptr;
        break;
    case fixture_mode_t::NO_DESTROY:
        plugin.destroy_engine = nullptr;
        break;
    case fixture_mode_t::NO_NAME_CALLBACK:
        plugin.get_plugin_name = nullptr;
        break;
    case fixture_mode_t::NO_VERSION_CALLBACK:
        plugin.get_plugin_version = nullptr;
        break;
    case fixture_mode_t::NO_OPTIONS:
        plugin.get_backend_options = nullptr;
        break;
    case fixture_mode_t::NO_MEMS:
        plugin.get_backend_mems = nullptr;
        break;
    case fixture_mode_t::NO_OPTIONAL:
        plugin.get_backend_options = nullptr;
        plugin.get_backend_mems = nullptr;
        break;
    case fixture_mode_t::VALID:
    case fixture_mode_t::NULL_INIT:
    case fixture_mode_t::THROW_INIT:
    case fixture_mode_t::NULL_NAME:
    case fixture_mode_t::EMPTY_NAME:
    case fixture_mode_t::NULL_VERSION:
    case fixture_mode_t::EMPTY_VERSION:
    case fixture_mode_t::NULL_ENGINE:
    case fixture_mode_t::THROW_CREATE:
    case fixture_mode_t::ENGINE_INIT_ERROR:
        break;
    }
    return &plugin;
}

#ifndef LOADER_FIXTURE_NO_FINI
extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {
    record(fixture_event_t::FINALIZE);
}
#endif
