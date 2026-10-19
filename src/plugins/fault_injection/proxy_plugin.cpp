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

#include "forwarding_engine.h"
#include "plugin_loader.h"
#include "backend/backend_plugin.h"
#include "common/configuration.h"
#include "common/nixl_log.h"

#include <cstdlib>
#include <cstdint>
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
    const auto config = toml::parse_file(file);
    const auto section = config["fault_injection"];
    const auto *table = section.as_table();
    if (!table) {
        throw std::invalid_argument("Missing [fault_injection] table");
    }
    for (const auto &[key, value] : *table) {
        const auto name = key.str();
        if (name != "schema_version" && name != "experiment_id" && name != "real_plugin_path" &&
            name != "rules") {
            throw std::invalid_argument("Unknown fault_injection key: " + std::string(name));
        }
    }
    if (!section["schema_version"].is_integer() ||
        section["schema_version"].value<int64_t>() != 1) {
        throw std::invalid_argument("fault_injection.schema_version must be integer 1");
    }
    const auto experiment = section["experiment_id"].value<std::string>();
    if (!experiment || experiment->empty()) {
        throw std::invalid_argument("fault_injection.experiment_id must be a nonempty string");
    }
    if (const auto rules = section["rules"]; rules) {
        if (!rules.is_array() || !rules.as_array()->empty()) {
            throw std::invalid_argument(
                "Fault injection rules are not supported by the inert proxy");
        }
    }
    const auto path = section["real_plugin_path"].value<std::string>();
    if (!path || !std::filesystem::path(*path).is_absolute()) {
        throw std::invalid_argument("fault_injection.real_plugin_path must be an absolute path");
    }
    return *path;
}

struct proxyState {
    explicit proxyState(const std::filesystem::path &path)
        : loader(path, shouldDeepBind()),
          name(loader.getName()),
          version(loader.getVersion()) {}

    nixl::fault::pluginLoader loader;
    std::string name;
    std::string version;
};

std::mutex state_mutex;
// Core may finalize plugins after DSO globals have been destroyed at process exit.
// Own the slot explicitly between init/fini instead of registering a static destructor.
std::shared_ptr<proxyState> *state = nullptr;
size_t initializations = 0;

std::shared_ptr<proxyState>
getState() {
    const std::lock_guard lock(state_mutex);
    if (!state) {
        throw std::runtime_error("Fault proxy is not initialized");
    }
    return *state;
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

nixlBackendPlugin plugin = {
    NIXL_PLUGIN_API_VERSION,
    createEngine,
    [](nixlBackendEngine *engine) { delete engine; },
    []() { return getState()->name.c_str(); },
    []() { return getState()->version.c_str(); },
    []() { return getState()->loader.getBackendOptions(); },
    []() { return getState()->loader.getBackendMems(); },
};

} // namespace

extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    try {
        const std::lock_guard lock(state_mutex);
        if (!state) {
            state = new std::shared_ptr<proxyState>(std::make_shared<proxyState>(readPluginPath()));
            NIXL_INFO << "Fault proxy initialized for " << (*state)->name << "; injection disabled";
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
        }
    }
    // Engine deleters keep the genuine plugin alive after the proxy's last finalization.
}
