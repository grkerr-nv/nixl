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

#include "backend_plugin_loader.h"

#include <dlfcn.h>

#include <exception>
#include <utility>

#include "backend/backend_plugin.h"
#include "common/nixl_log.h"

namespace {

using plugin_init_fn_t = nixlBackendPlugin *(*)();
using plugin_fini_fn_t = void (*)();

bool
hasRequiredCallbacks(const nixlBackendPlugin &plugin) {
    return plugin.create_engine != nullptr && plugin.destroy_engine != nullptr &&
        plugin.get_plugin_name != nullptr && plugin.get_plugin_version != nullptr &&
        plugin.get_backend_options != nullptr && plugin.get_backend_mems != nullptr;
}

template<typename Function>
Function
loadSymbol(void *handle, const char *symbol, const std::filesystem::path &path) {
    dlerror();
    auto function = reinterpret_cast<Function>(dlsym(handle, symbol));
    const char *error = dlerror();
    if (error != nullptr || function == nullptr) {
        NIXL_ERROR << "Failed to find " << symbol << " in '" << path.string()
                   << "': " << (error == nullptr ? "symbol not found" : error);
        return nullptr;
    }
    return function;
}

void
closePlugin(void *handle, plugin_fini_fn_t fini, bool initialized) noexcept {
    if (initialized) {
        try {
            fini();
        }
        catch (const std::exception &error) {
            NIXL_ERROR << "Backend plugin finalization threw: " << error.what();
        }
        catch (...) {
            NIXL_ERROR << "Backend plugin finalization threw an unknown exception";
        }
    }
    if (handle != nullptr) {
        dlclose(handle);
    }
}

} // namespace

struct nixlBackendPluginLoader::state {
    state(void *handle, nixlBackendPlugin *plugin, plugin_fini_fn_t fini)
        : handle_(handle),
          plugin_(plugin),
          fini_(fini) {}

    ~state() {
        closePlugin(handle_, fini_, true);
    }

    void *handle_;
    nixlBackendPlugin *plugin_;
    plugin_fini_fn_t fini_;
};

nixlBackendPluginLoader::engineDeleter::engineDeleter(std::shared_ptr<state> state)
    : state_(std::move(state)) {}

void
nixlBackendPluginLoader::engineDeleter::operator()(nixlBackendEngine *engine) const noexcept {
    if (engine == nullptr) {
        return;
    }
    try {
        state_->plugin_->destroy_engine(engine);
    }
    catch (const std::exception &error) {
        NIXL_ERROR << "Backend engine destruction threw: " << error.what();
    }
    catch (...) {
        NIXL_ERROR << "Backend engine destruction threw an unknown exception";
    }
}

std::unique_ptr<nixlBackendPluginLoader>
nixlBackendPluginLoader::load(const std::filesystem::path &path) {
    if (!path.is_absolute()) {
        NIXL_ERROR << "Backend plugin path must be absolute: '" << path.string() << "'";
        return nullptr;
    }

    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        NIXL_ERROR << "Failed to load backend plugin '" << path.string() << "': " << dlerror();
        return nullptr;
    }

    plugin_init_fn_t init = loadSymbol<plugin_init_fn_t>(handle, "nixl_plugin_init", path);
    plugin_fini_fn_t fini = loadSymbol<plugin_fini_fn_t>(handle, "nixl_plugin_fini", path);
    if (init == nullptr || fini == nullptr) {
        closePlugin(handle, fini, false);
        return nullptr;
    }

    nixlBackendPlugin *plugin = nullptr;
    try {
        plugin = init();
    }
    catch (const std::exception &error) {
        NIXL_ERROR << "Backend plugin initialization threw: " << error.what();
    }
    catch (...) {
        NIXL_ERROR << "Backend plugin initialization threw an unknown exception";
    }

    if (plugin == nullptr) {
        NIXL_ERROR << "Backend plugin initialization failed for '" << path.string() << "'";
        closePlugin(handle, fini, false);
        return nullptr;
    }
    if (plugin->api_version != NIXL_PLUGIN_API_VERSION) {
        NIXL_ERROR << "Backend plugin API version mismatch for '" << path.string() << "': expected "
                   << NIXL_PLUGIN_API_VERSION << ", got " << plugin->api_version;
        closePlugin(handle, fini, true);
        return nullptr;
    }
    if (!hasRequiredCallbacks(*plugin)) {
        NIXL_ERROR << "Backend plugin '" << path.string() << "' is missing a required callback";
        closePlugin(handle, fini, true);
        return nullptr;
    }

    return std::unique_ptr<nixlBackendPluginLoader>(
        new nixlBackendPluginLoader(std::make_shared<state>(handle, plugin, fini)));
}

nixlBackendPluginLoader::nixlBackendPluginLoader(std::shared_ptr<state> state)
    : state_(std::move(state)) {}

nixlBackendPluginLoader::engine_ptr_t
nixlBackendPluginLoader::createEngine(const nixlBackendInitParams *init_params) const {
    nixlBackendEngine *engine = nullptr;
    try {
        engine = state_->plugin_->create_engine(init_params);
    }
    catch (const std::exception &error) {
        NIXL_ERROR << "Backend engine creation threw: " << error.what();
    }
    catch (...) {
        NIXL_ERROR << "Backend engine creation threw an unknown exception";
    }
    return engine_ptr_t(engine, engineDeleter(state_));
}

std::string
nixlBackendPluginLoader::getName() const {
    const char *name = state_->plugin_->get_plugin_name();
    return name == nullptr ? std::string() : name;
}

std::string
nixlBackendPluginLoader::getVersion() const {
    const char *version = state_->plugin_->get_plugin_version();
    return version == nullptr ? std::string() : version;
}

nixl_b_params_t
nixlBackendPluginLoader::getBackendOptions() const {
    return state_->plugin_->get_backend_options();
}

nixl_mem_list_t
nixlBackendPluginLoader::getBackendMems() const {
    return state_->plugin_->get_backend_mems();
}
