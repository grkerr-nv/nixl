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

// What the inert proxy uses from the configuration: where the genuine plugin lives, and
// the experiment the run log must name.
struct proxyConfig {
    std::filesystem::path realPluginPath;
    std::string experimentId;
};

proxyConfig
readConfig() {
    const char *file = std::getenv("NIXL_FAULT_CONFIG_FILE");
    if (!file || !*file) {
        throw std::invalid_argument(
            "NIXL_FAULT_CONFIG_FILE must specify real_plugin_path configuration");
    }
    const auto config = nixl::fault::faultConfig::load(file);
    if (!config.getRules().empty()) {
        throw std::invalid_argument("Fault injection rules are not supported by the inert proxy");
    }
    return {config.getRealPluginPath(), config.getExperimentId()};
}

struct proxyState {
    explicit proxyState(proxyConfig config)
        : experimentId(std::move(config.experimentId)),
          loader(config.realPluginPath, shouldDeepBind()) {}

    std::string experimentId;
    nixl::fault::pluginLoader loader;
};

// What core reports for an accessor a plugin does not provide. The proxy matches it
// whenever it has no genuine plugin to describe.
constexpr const char *unknown_identity = "unknown";

// Serializes plugin init/fini and access to the shared state slot and initialization count.
// Hold while copying or changing state, and across the genuine plugin's own
// initialization and finalization so the two cannot overlap.
// Engine forwarding does not take this lock; identity getters use atomics.
std::mutex state_mutex;
// Core may finalize plugins after DSO globals have been destroyed at process exit.
// Own the slot explicitly between init/fini instead of registering a static destructor.
std::shared_ptr<proxyState> *state = nullptr;
size_t initializations = 0;
// Identity the plugin ABI returns as const char *. Core reads it through a handle it
// still holds after the last nixl_plugin_fini(), so it cannot live in proxyState.
std::atomic<const char *> plugin_name{unknown_identity};
std::atomic<const char *> plugin_version{unknown_identity};

// Keeps one immutable name/version pair so returned pointers survive re-initialization.
// A loaded proxy cannot change identity. Call under state_mutex.
const std::pair<std::string, std::string> &
retainIdentity(const nixl::fault::pluginLoader &loader) {
    const char *name = loader.getName();
    const char *version = loader.getVersion();
    static const std::pair<std::string, std::string> identity{name, version};
    if (identity.first != name || identity.second != version) {
        throw std::invalid_argument("Fault proxy cannot change backend name or version on "
                                    "re-initialization; start a new process");
    }
    return identity;
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

// Core calls the plugin ABI without an exception handler, and the genuine plugin's
// callbacks may throw. Log the failure and report an empty result instead of unwinding
// across the boundary.
template<class T>
auto
callGuarded(const char *failure, T call) -> decltype(call()) {
    try {
        return call();
    }
    catch (const std::exception &error) {
        NIXL_ERROR << failure << ": " << error.what();
    }
    catch (...) {
        NIXL_ERROR << failure << ": unknown exception";
    }
    return {};
}

nixlBackendEngine *
createEngine(const nixlBackendInitParams *params) {
    return callGuarded("Cannot create fault proxy engine", [params]() -> nixlBackendEngine * {
        const auto owner = getState();
        // params->type is the name core looked up to find this library. Refuse to pass off
        // some other backend under it; forwardingEngine cannot tell, since it compares the
        // wrapped engine against the same params.
        if (params && params->type != owner->loader.getName()) {
            throw std::invalid_argument("Fault proxy was discovered as '" + params->type +
                                        "' but real_plugin_path provides '" +
                                        owner->loader.getName() + "'");
        }
        // Move the unique owner with its genuine plugin deleter and lifetime.
        return new nixl::fault::forwardingEngine(params, owner->loader.createEngine(params));
    });
}

nixl_b_params_t
backendOptions() {
    return callGuarded("Cannot read fault proxy backend options", [] {
        const auto owner = tryGetState();
        return owner ? owner->loader.getBackendOptions() : nixl_b_params_t{};
    });
}

nixl_mem_list_t
backendMems() {
    return callGuarded("Cannot read fault proxy backend memory types", [] {
        const auto owner = tryGetState();
        return owner ? owner->loader.getBackendMems() : nixl_mem_list_t{};
    });
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
    return callGuarded("Cannot initialize fault proxy", []() -> nixlBackendPlugin * {
        const std::lock_guard lock(state_mutex);
        if (!state) {
            auto created = std::make_shared<proxyState>(readConfig());
            // Retain both before publishing either, so identity never goes half updated.
            const auto &[name, version] = retainIdentity(created->loader);
            plugin_name.store(name.c_str(), std::memory_order_release);
            plugin_version.store(version.c_str(), std::memory_order_release);
            state = new std::shared_ptr<proxyState>(std::move(created));
            NIXL_INFO << "Fault proxy initialized for " << name << " as experiment '"
                      << (*state)->experimentId << "'; injection disabled";
        }
        ++initializations;
        return &plugin;
    });
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {
    const std::lock_guard lock(state_mutex);
    if (initializations && --initializations == 0) {
        // Hold the lock through teardown, as init does through the genuine plugin's
        // initialization. Releasing first would let a concurrent init re-initialize the
        // genuine library while this thread finalizes it.
        const std::shared_ptr<proxyState> previous = std::move(*state);
        delete std::exchange(state, nullptr);
        // Describe the finalized proxy as core describes a missing accessor. Strings
        // handed out earlier stay valid; the retained pair is never replaced.
        plugin_name.store(unknown_identity, std::memory_order_release);
        plugin_version.store(unknown_identity, std::memory_order_release);
    }
    // Engine deleters keep the genuine plugin alive after the proxy's last finalization,
    // and finalize it from whichever thread drops the last engine. That teardown does not
    // take this lock, so it can still overlap a later initialization.
}
