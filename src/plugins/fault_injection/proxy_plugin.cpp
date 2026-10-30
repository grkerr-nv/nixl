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

#include "fault_config.h"
#include "forwarding_engine.h"
#include "plugin_loader.h"
#include "backend/backend_plugin.h"
#include "common/configuration.h"
#include "common/nixl_log.h"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// Match core's UCX policy using the filename selected by plugin discovery.
bool
shouldDeepBind() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&shouldDeepBind), &info) || !info.dli_fname ||
        std::filesystem::path(info.dli_fname).filename() != "libplugin_UCX.so") {
        return false;
    }
    try {
        return nixl::config::getValueDefaulted<bool>("NIXL_UCX_DEEPBIND", false);
    }
    catch (const std::exception &error) {
        NIXL_WARN << "Invalid NIXL_UCX_DEEPBIND value, enabling RTLD_DEEPBIND: " << error.what();
        return true;
    }
}

std::filesystem::path
readPluginPath() {
    const char *file = std::getenv("NIXL_FAULT_CONFIG_FILE");
    if (!file || !*file) {
        throw std::invalid_argument(
            "NIXL_FAULT_CONFIG_FILE must specify real_plugin_path configuration");
    }
    const auto config = nixl::fault::faultConfig::load(file);
    // The proxy forwards every call unchanged, so a rule it cannot execute must not look
    // like it took effect.
    if (!config.getRules().empty()) {
        throw std::invalid_argument("Fault injection rules are not supported by the inert proxy");
    }
    return config.getRealPluginPath();
}

struct proxyState {
    explicit proxyState(const std::filesystem::path &path) : loader(path, shouldDeepBind()) {}

    nixl::fault::pluginLoader loader;
};

// What core reports for an accessor a plugin does not provide. The proxy matches it
// whenever it has no genuine plugin to describe.
constexpr const char *unknown_identity = "unknown";

std::mutex state_mutex;
// Core may finalize plugins after DSO globals have been destroyed at process exit.
// Own the slot explicitly between init/fini instead of registering a static destructor.
std::shared_ptr<proxyState> *state = nullptr;
size_t initializations = 0;
// Identity the plugin ABI returns as const char *. Core reads it through a handle it
// still holds after the last nixl_plugin_fini(), so it cannot live in proxyState.
std::atomic<const char *> plugin_name{unknown_identity};
std::atomic<const char *> plugin_version{unknown_identity};

// Keeps identity strings alive as long as the library is loaded, as core's own plugins do.
// Appends rather than replacing, so a pointer returned before a re-initialization stays
// valid. Call under state_mutex.
const char *
retainIdentity(const std::string &value) {
    static std::deque<std::string> values;
    return values.emplace_back(value).c_str();
}

// Accessors run on the plugin ABI, which core calls without an exception handler. Report
// an uninitialized proxy instead of throwing across it.
std::shared_ptr<proxyState>
tryGetState() {
    const std::lock_guard lock(state_mutex);
    return state ? *state : nullptr;
}

std::shared_ptr<proxyState>
getState() {
    auto owner = tryGetState();
    if (!owner) {
        throw std::runtime_error("Fault proxy is not initialized");
    }
    return owner;
}

nixlBackendEngine *
createEngine(const nixlBackendInitParams *params) {
    try {
        const auto owner = getState();
        // Move the unique owner with its genuine plugin deleter and lifetime.
        return new nixl::fault::forwardingEngine(params, owner->loader.createEngine(params));
    }
    catch (const std::exception &error) {
        NIXL_ERROR << "Cannot create fault proxy engine: " << error.what();
    }
    catch (...) {
        NIXL_ERROR << "Cannot create fault proxy engine: unknown exception";
    }
    return nullptr;
}

nixl_b_params_t
backendOptions() {
    const auto owner = tryGetState();
    return owner ? owner->loader.getBackendOptions() : nixl_b_params_t{};
}

nixl_mem_list_t
backendMems() {
    const auto owner = tryGetState();
    return owner ? owner->loader.getBackendMems() : nixl_mem_list_t{};
}

nixlBackendPlugin plugin = {
    NIXL_PLUGIN_API_VERSION,
    createEngine,
    [](nixlBackendEngine *engine) { delete engine; },
    []() { return plugin_name.load(std::memory_order_acquire); },
    []() { return plugin_version.load(std::memory_order_acquire); },
    backendOptions,
    backendMems,
};

} // namespace

extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    try {
        const std::lock_guard lock(state_mutex);
        if (!state) {
            auto created = std::make_shared<proxyState>(readPluginPath());
            // Retain both before publishing either, so identity never goes half updated.
            const char *name = retainIdentity(created->loader.getName());
            const char *version = retainIdentity(created->loader.getVersion());
            plugin_name.store(name, std::memory_order_release);
            plugin_version.store(version, std::memory_order_release);
            state = new std::shared_ptr<proxyState>(std::move(created));
            NIXL_INFO << "Fault proxy initialized for " << name << "; injection disabled";
        }
        ++initializations;
        return &plugin;
    }
    catch (const std::exception &error) {
        NIXL_ERROR << "Cannot initialize fault proxy: " << error.what();
    }
    catch (...) {
        NIXL_ERROR << "Cannot initialize fault proxy: unknown exception";
    }
    return nullptr;
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {
    std::shared_ptr<proxyState> previous;
    {
        const std::lock_guard lock(state_mutex);
        if (initializations && --initializations == 0) {
            previous = std::move(*state);
            delete std::exchange(state, nullptr);
            // Describe the finalized proxy as core describes a missing accessor. Strings
            // handed out earlier stay valid; retainIdentity never frees them.
            plugin_name.store(unknown_identity, std::memory_order_release);
            plugin_version.store(unknown_identity, std::memory_order_release);
        }
    }
    // Engine deleters keep the genuine plugin alive after the proxy's last finalization.
}
