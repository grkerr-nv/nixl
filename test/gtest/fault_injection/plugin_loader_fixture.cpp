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
#include "../mocks/gmock_engine.h"

#include <stdexcept>

namespace {
using loader_fixture::Mode;
using loader_fixture::Event;
Mode mode = Mode::Valid;
loader_fixture::Observer observer;

void
record(Event event, const nixlBackendInitParams *params = nullptr) {
    if (observer.record) {
        observer.record(observer.context, event, params);
    }
}

class FixtureEngine : public mocks::GMockBackendEngine {
public:
    explicit FixtureEngine(const nixlBackendInitParams *params)
        : mocks::GMockBackendEngine(params) {
        initErr = mode == Mode::EngineInitError;
    }

    ~FixtureEngine() override {
        record(Event::EngineDestroyed);
    }
};

nixlBackendEngine *
create(const nixlBackendInitParams *params) {
    record(Event::Create, params);
    if (mode == Mode::NullEngine) {
        return nullptr;
    }
    if (mode == Mode::ThrowCreate) {
        throw std::runtime_error("fixture create failed");
    }
    return new testing::NiceMock<FixtureEngine>(params);
}

void
destroy(nixlBackendEngine *engine) {
    record(Event::DestroyCallback);
    delete engine;
}

const char *
name() {
    if (mode == Mode::NullName) {
        return nullptr;
    }
    return mode == Mode::EmptyName ? "" : "LOADER_FIXTURE";
}

const char *
version() {
    if (mode == Mode::NullVersion) {
        return nullptr;
    }
    return mode == Mode::EmptyVersion ? "" : "1.2.3";
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
loader_fixture_configure(Mode next, loader_fixture::Observer nextObserver) {
    mode = next;
    observer = nextObserver;
}

extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    record(Event::Init);
    if (mode == Mode::NullInit) {
        return nullptr;
    }
    if (mode == Mode::ThrowInit) {
        throw std::runtime_error("fixture init failed");
    }
    static nixlBackendPlugin plugin;
    plugin = {NIXL_PLUGIN_API_VERSION, create, destroy, name, version, options, mems};
    switch (mode) {
    case Mode::OldApi:
        plugin.api_version = NIXL_PLUGIN_API_VERSION - 1;
        break;
    case Mode::NoCreate:
        plugin.create_engine = nullptr;
        break;
    case Mode::NoDestroy:
        plugin.destroy_engine = nullptr;
        break;
    case Mode::NoNameCallback:
        plugin.get_plugin_name = nullptr;
        break;
    case Mode::NoVersionCallback:
        plugin.get_plugin_version = nullptr;
        break;
    case Mode::NoOptions:
        plugin.get_backend_options = nullptr;
        break;
    case Mode::NoMems:
        plugin.get_backend_mems = nullptr;
        break;
    case Mode::NoOptional:
        plugin.get_backend_options = nullptr;
        plugin.get_backend_mems = nullptr;
        break;
    default:
        break;
    }
    return &plugin;
}

#ifndef LOADER_FIXTURE_NO_FINI
extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {
    record(Event::Finalize);
}
#endif
