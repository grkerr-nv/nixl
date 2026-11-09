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
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
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
        // On lookup failure, defer to the loaded entrypoint's object identity.
        if (std::filesystem::equivalent(path, info.dli_fname, ec) && !ec) {
            throw std::invalid_argument("Genuine plugin path refers to the proxy itself: " +
                                        path.string());
        }
    }
} // namespace

struct pluginLoader::pluginState {
    using finalize_t = void (*)();

    // Live states keyed by dlopen handle. An entry exists only while it has owners, and
    // owners change only under the mutex, so a lookup never sees a state mid-teardown.
    struct registryState {
        // One lock for every genuine library: it covers dlopen, the plugin's
        // initialization and validation, and finalization from any thread that drops the
        // last owner.
        std::mutex mutex;
        std::unordered_map<void *, std::unique_ptr<pluginState>> plugins;
    };

    static std::shared_ptr<registryState>
    getRegistry() {
        // Live owners retain the registry past this static owner's destruction during shutdown.
        static const auto registry = std::make_shared<registryState>();
        return registry;
    }

    // One per loader. Engine deleters share it, so it expires after the loader and all
    // of its engines. The last owner of a state finalizes it under the registry lock.
    struct ownerRef {
        explicit ownerRef(std::shared_ptr<registryState> registry)
            : registry(std::move(registry)) {}

        ownerRef(const ownerRef &) = delete;
        ownerRef &
        operator=(const ownerRef &) = delete;

        ~ownerRef() {
            if (!state) {
                return;
            }
            const std::lock_guard lock(registry->mutex);
            state->owners -= 1;
            if (state->owners == 0) {
                registry->plugins.erase(state->handle);
            }
        }

        std::shared_ptr<registryState> registry;
        pluginState *state = nullptr;
    };

    explicit pluginState(std::filesystem::path path) : path(std::move(path)) {}

    pluginState(const pluginState &) = delete;
    pluginState &
    operator=(const pluginState &) = delete;

    // Runs under the registry lock, so finalization cannot overlap a later initialization.
    ~pluginState() {
        if (plugin && finalize) {
            try {
                finalize();
            }
            catch (const std::exception &error) {
                NIXL_ERROR << "Genuine plugin finalization threw in " << path << ": "
                           << error.what();
            }
            catch (...) {
                NIXL_ERROR << "Genuine plugin finalization threw in " << path
                           << ": unknown exception";
            }
        }
        if (handle) {
            dlclose(handle);
        }
    }

    // Each successful init owns one matching fini, including validation failures.
    // The path is the one that first loaded the library; aliases share this state.
    std::filesystem::path path;
    void *handle = nullptr;
    nixlBackendPlugin *plugin = nullptr;
    finalize_t finalize = nullptr;
    size_t owners = 0;
};

pluginLoader::pluginLoader(const std::filesystem::path &path, bool deepbind) : path_(path) {
    if (!path.is_absolute()) {
        throw std::invalid_argument("Genuine plugin path must be absolute: " + path.string());
    }

    rejectSelfLoad(path);

    auto registry = pluginState::getRegistry();
    // Allocate before locking: releasing an owner takes the lock.
    auto owner = std::make_shared<pluginState::ownerRef>(registry);
    const std::lock_guard lock(registry->mutex);
    // Declared after the lock, so a failed load finalizes and closes before unlocking.
    auto state = std::make_unique<pluginState>(path);
    // Core's path loader is private; keep this loader usable with unmodified NIXL.
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
        const char *error = dlerror();
        throw std::runtime_error("Cannot load genuine plugin '" + path.string() +
                                 "': " + (error ? error : "unknown error"));
    }

    // nixl_plugin_init takes no arguments and returns the plugin interface.
    using initialize_function_t = nixlBackendPlugin *();
    dlerror(); // Clear stale errors before looking up the entrypoint.
    const auto initialize =
        reinterpret_cast<initialize_function_t *>(dlsym(state->handle, "nixl_plugin_init"));
    if (const char *error = dlerror(); error || !initialize) {
        throw std::runtime_error("Cannot resolve nixl_plugin_init in '" + path.string() +
                                 "': " + (error ? error : "null entrypoint"));
    }
    // Object identity survives deleted aliases and changes to the working directory.
    Dl_info proxy_info{}, plugin_info{};
    if (!dladdr(reinterpret_cast<void *>(&rejectSelfLoad), &proxy_info) ||
        !dladdr(reinterpret_cast<void *>(initialize), &plugin_info) || !proxy_info.dli_fbase ||
        !plugin_info.dli_fbase) {
        throw std::runtime_error("Cannot identify genuine plugin entrypoint: " + path.string());
    }
    if (proxy_info.dli_fbase == plugin_info.dli_fbase) {
        throw std::invalid_argument("Genuine plugin path refers to the proxy itself: " +
                                    path.string());
    }
    // dlopen identifies the loaded object even through symlinks and hard links.
    if (const auto found = registry->plugins.find(state->handle);
        found != registry->plugins.end()) {
        // The uninitialized local state drops only this call's extra dlopen reference.
        owner->state = found->second.get();
        owner->state->owners += 1;
        auto *shared = owner->state;
        state_ = std::shared_ptr<pluginState>(std::move(owner), shared);
        return;
    }
    // Fini is optional, as in the core plugin manager.
    state->finalize =
        reinterpret_cast<pluginState::finalize_t>(dlsym(state->handle, "nixl_plugin_fini"));
    dlerror(); // Discard errors from an absent optional finalizer.
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
    auto *shared = state.get();
    // If emplace throws, whichever pointer holds the state finalizes it under the lock.
    registry->plugins.emplace(shared->handle, std::move(state));
    shared->owners = 1;
    owner->state = shared;
    state_ = std::shared_ptr<pluginState>(std::move(owner), shared);
}

const char *
pluginLoader::getName() const {
    return state_->plugin->get_plugin_name();
}

const char *
pluginLoader::getVersion() const {
    return state_->plugin->get_plugin_version();
}

nixl_b_params_t
pluginLoader::getBackendOptions() const {
    const auto callback = state_->plugin->get_backend_options;
    return callback ? callback() : nixl_b_params_t{};
}

nixl_mem_list_t
pluginLoader::getBackendMems() const {
    const auto callback = state_->plugin->get_backend_mems;
    return callback ? callback() : nixl_mem_list_t{};
}

engine_ptr_t
pluginLoader::createEngine(const nixlBackendInitParams *params) const {
    if (!params) {
        throw std::invalid_argument("Backend initialization parameters are required");
    }
    // Construct the owner first so no allocation can leak a successfully created engine.
    engine_ptr_t engine(nullptr, engineDeleter(state_->plugin->destroy_engine, state_));
    try {
        engine.reset(state_->plugin->create_engine(params));
    }
    catch (const std::exception &error) {
        throw std::runtime_error("Genuine plugin engine creation threw in '" + path_.string() +
                                 "': " + error.what());
    }
    if (!engine) {
        throw std::runtime_error("Genuine plugin engine creation failed: " + path_.string());
    }
    return engine;
}

} // namespace nixl::fault
