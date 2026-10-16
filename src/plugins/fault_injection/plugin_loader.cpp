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

#include "backend/backend_plugin.h"
#include "common/nixl_log.h"

#include <dlfcn.h>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace nixl::fault {

namespace {
    // Reject a path naming the shared object that contains this loader, i.e. the proxy itself.
    // dlopen would return our own handle and nixl_plugin_init would recurse.
    void
    rejectSelfLoad(const std::filesystem::path &path) {
        Dl_info info;
        if (!dladdr(reinterpret_cast<void *>(&rejectSelfLoad), &info) || !info.dli_fname) {
            return;
        }
        std::error_code ec;
        // On failure, skip the guard and let dlopen report the real problem.
        if (std::filesystem::equivalent(path, info.dli_fname, ec) && !ec) {
            throw std::invalid_argument("Genuine plugin path refers to the proxy itself: " +
                                        path.string());
        }
    }
} // namespace

struct PluginLoader::State {
    using Finalize = void (*)();

    explicit State(std::filesystem::path path) : path(std::move(path)) {}

    ~State() {
        if (plugin && finalize) {
            finalize();
        }
        if (handle) {
            dlclose(handle);
        }
    }

    // Each successful init owns one matching fini, including validation failures.
    std::filesystem::path path;
    void *handle = nullptr;
    nixlBackendPlugin *plugin = nullptr;
    Finalize finalize = nullptr;
};

PluginLoader::PluginLoader(const std::filesystem::path &path, bool deepbind) {
    if (!path.is_absolute()) {
        throw std::invalid_argument("Genuine plugin path must be absolute: " + path.string());
    }

    rejectSelfLoad(path);

    auto state = std::make_shared<State>(path);
    // Match core's NODELETE workaround for Abseil/TLS unloading on older glibc.
    // We still balance dlopen/dlclose and call the plugin finalizer at logical unload.
    int flags = RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE;
    if (deepbind) {
#ifdef RTLD_DEEPBIND
        flags |= RTLD_DEEPBIND;
#else
        // Same fallback as core: warn and load without it.
        NIXL_WARN << "RTLD_DEEPBIND requested for " << path << " but is not supported";
#endif
    }
    state->handle = dlopen(path.c_str(), flags);
    if (!state->handle) {
        throw std::runtime_error("Cannot load genuine plugin '" + path.string() +
                                 "': " + dlerror());
    }

    // nixl_plugin_init takes no arguments and returns the plugin interface.
    using InitializeFunction = nixlBackendPlugin *();
    dlerror(); // Clear stale errors before looking up the entrypoint.
    const auto initialize =
        reinterpret_cast<InitializeFunction *>(dlsym(state->handle, "nixl_plugin_init"));
    if (const char *error = dlerror(); error || !initialize) {
        throw std::runtime_error("Cannot resolve nixl_plugin_init in '" + path.string() +
                                 "': " + (error ? error : "null entrypoint"));
    }
    // Fini is optional, as in the core plugin manager.
    state->finalize = reinterpret_cast<State::Finalize>(dlsym(state->handle, "nixl_plugin_fini"));
    try {
        state->plugin = initialize();
    }
    catch (const std::exception &error) {
        throw std::runtime_error("Genuine plugin initialization threw in '" + path.string() +
                                 "': " + error.what());
    }
    if (!state->plugin) {
        throw std::runtime_error("Genuine plugin initialization failed: " + path.string());
    }

    const auto &plugin = *state->plugin;
    if (plugin.api_version != NIXL_PLUGIN_API_VERSION) {
        throw std::runtime_error("Genuine plugin API version mismatch in '" + path.string() +
                                 "': expected " + std::to_string(NIXL_PLUGIN_API_VERSION) +
                                 ", got " + std::to_string(plugin.api_version));
    }
    if (!plugin.create_engine || !plugin.destroy_engine || !plugin.get_plugin_name ||
        !plugin.get_plugin_version) {
        throw std::runtime_error("Genuine plugin is missing required callbacks: " + path.string());
    }
    const char *name = plugin.get_plugin_name();
    const char *version = plugin.get_plugin_version();
    if (!name || !*name || !version || !*version) {
        throw std::runtime_error("Genuine plugin has an empty name or version: " + path.string());
    }
    state_ = std::move(state);
}

PluginLoader::EngineDeleter::EngineDeleter(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

void
PluginLoader::EngineDeleter::operator()(nixlBackendEngine *engine) const noexcept {
    if (engine) {
        state_->plugin->destroy_engine(engine);
    }
}

std::string
PluginLoader::getName() const {
    return state_->plugin->get_plugin_name();
}

std::string
PluginLoader::getVersion() const {
    return state_->plugin->get_plugin_version();
}

nixl_b_params_t
PluginLoader::getBackendOptions() const {
    const auto callback = state_->plugin->get_backend_options;
    return callback ? callback() : nixl_b_params_t{};
}

nixl_mem_list_t
PluginLoader::getBackendMems() const {
    const auto callback = state_->plugin->get_backend_mems;
    return callback ? callback() : nixl_mem_list_t{};
}

PluginLoader::EnginePtr
PluginLoader::createEngine(const nixlBackendInitParams *params) const {
    if (!params) {
        throw std::invalid_argument("Backend initialization parameters are required");
    }
    // Construct the owner first so no allocation can leak a successfully created engine.
    EnginePtr engine(nullptr, EngineDeleter(state_));
    try {
        engine.reset(state_->plugin->create_engine(params));
    }
    catch (const std::exception &error) {
        throw std::runtime_error("Genuine plugin engine creation threw in '" +
                                 state_->path.string() + "': " + error.what());
    }
    if (!engine) {
        throw std::runtime_error("Genuine plugin engine creation failed: " + state_->path.string());
    }
    return engine;
}

} // namespace nixl::fault
