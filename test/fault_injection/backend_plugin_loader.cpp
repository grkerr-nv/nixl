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
#include <type_traits>
#include <utility>

#include "backend/backend_plugin.h"
#include "common/nixl_log.h"

namespace {

using plugin_init_fn_t = nixlBackendPlugin *(*)();
using plugin_fini_fn_t = void (*)();

bool
hasRequiredCallbacks(const nixlBackendPlugin &plugin) {
    // get_backend_options and get_backend_mems are left out on purpose:
    // nixlBackendPluginHandle null-checks them at the call sites rather than
    // demanding them, and rejecting here would turn away plugins NIXL accepts.
    return plugin.create_engine != nullptr && plugin.destroy_engine != nullptr &&
        plugin.get_plugin_name != nullptr && plugin.get_plugin_version != nullptr;
}

// Plugin code is foreign to the harness, so every call into it reports rather
// than propagates, and yields an empty result.
template<typename Function>
std::invoke_result_t<Function>
guarded(const char *operation, Function function) {
    try {
        return function();
    }
    catch (const std::exception &error) {
        NIXL_ERROR << "Backend plugin " << operation << " threw: " << error.what();
    }
    catch (...) {
        NIXL_ERROR << "Backend plugin " << operation << " threw an unknown exception";
    }
    return {};
}

int
dlopenFlags(nixlBackendPluginLoader::symbolBinding binding) {
    // RTLD_NODELETE matches nixlPluginManager::loadPluginFromPath: plugins link
    // Abseil, whose thread_local and static initialization are unsafe to unload,
    // so the mapping has to survive dlclose.
    int flags = RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE;
    if (binding == nixlBackendPluginLoader::symbolBinding::deepBind) {
#ifdef RTLD_DEEPBIND
        flags |= RTLD_DEEPBIND;
#else
        NIXL_WARN << "RTLD_DEEPBIND requested but is not supported on this platform";
#endif
    }
    return flags;
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
nixlBackendPluginLoader::load(const std::filesystem::path &path, symbolBinding binding) {
    if (!path.is_absolute()) {
        NIXL_ERROR << "Backend plugin path must be absolute: '" << path.string() << "'";
        return nullptr;
    }

    void *handle = dlopen(path.c_str(), dlopenFlags(binding));
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

    nixlBackendPlugin *plugin = guarded("initialization", [init] { return init(); });

    if (plugin == nullptr) {
        NIXL_ERROR << "Backend plugin initialization failed for '" << path.string() << "'";
        closePlugin(handle, fini, false);
        return nullptr;
    }
    if (plugin->api_version != NIXL_PLUGIN_API_VERSION) {
        NIXL_ERROR << "Backend plugin API version mismatch for '" << path.string() << "': expected "
                   << NIXL_PLUGIN_API_VERSION << ", got " << plugin->api_version;
        // Skip nixl_plugin_fini: its ABI belongs to a version we just rejected.
        closePlugin(handle, fini, false);
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
    nixlBackendEngine *engine = guarded("engine creation", [this, init_params] {
        return state_->plugin_->create_engine(init_params);
    });
    return engine_ptr_t(engine, engineDeleter(state_));
}

std::string
nixlBackendPluginLoader::getName() const {
    return guarded("name query", [this] {
        const char *name = state_->plugin_->get_plugin_name();
        return name == nullptr ? std::string() : std::string(name);
    });
}

std::string
nixlBackendPluginLoader::getVersion() const {
    return guarded("version query", [this] {
        const char *version = state_->plugin_->get_plugin_version();
        return version == nullptr ? std::string() : std::string(version);
    });
}

nixl_b_params_t
nixlBackendPluginLoader::getBackendOptions() const {
    if (state_->plugin_->get_backend_options == nullptr) {
        return {};
    }
    return guarded("options query", [this] { return state_->plugin_->get_backend_options(); });
}

nixl_mem_list_t
nixlBackendPluginLoader::getBackendMems() const {
    if (state_->plugin_->get_backend_mems == nullptr) {
        return {};
    }
    return guarded("memory list query", [this] { return state_->plugin_->get_backend_mems(); });
}
