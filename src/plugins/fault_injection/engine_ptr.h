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

#ifndef NIXL_SRC_PLUGINS_FAULT_INJECTION_ENGINE_PTR_H
#define NIXL_SRC_PLUGINS_FAULT_INJECTION_ENGINE_PTR_H

#include "backend/backend_engine.h"
#include "common/nixl_log.h"

#include <dlfcn.h>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nixl::fault {

// Plugin ownership keeps its callbacks valid through engine destruction.
class engineDeleter {
public:
    using destroy_t = void (*)(nixlBackendEngine *);

    // Engines a plugin created. Destroy through its callback, not operator delete.
    engineDeleter(destroy_t destroy, std::shared_ptr<void> plugin_owner)
        : destroy_(destroy),
          pluginOwner_(std::move(plugin_owner)) {
        if (!destroy_ || !pluginOwner_) {
            throw std::invalid_argument("A plugin destroy callback and shared owner are required");
        }
    }

    // Engines the caller allocated directly, including mocks. Never adopt a plugin
    // engine with this: operator delete is not the plugin's deleter, and nothing
    // keeps the plugin loaded through destruction.
    static engineDeleter
    forOwnedEngine() {
        return engineDeleter(deleteEngine);
    }

    void
    operator()(nixlBackendEngine *engine) const noexcept {
        try {
            destroy_(engine);
        }
        catch (const std::exception &error) {
            NIXL_ERROR << "Backend engine destruction threw in '" << getPluginPath()
                       << "': " << error.what();
        }
        catch (...) {
            NIXL_ERROR << "Backend engine destruction threw in '" << getPluginPath()
                       << "': unknown exception";
        }
    }

private:
    // Ownerless construction, reachable only through forOwnedEngine(). Private because a
    // deleter without a plugin owner is safe for exactly one case; the public API names
    // both choices so neither is picked by default.
    explicit engineDeleter(destroy_t destroy) : destroy_(destroy) {}

    // Cleanup for directly allocated engines; plugin engines use their callback.
    static void
    deleteEngine(nixlBackendEngine *engine) {
        delete engine;
    }

    // pluginOwner_ keeps dli_fname valid throughout the destruction diagnostic.
    const char *
    getPluginPath() const noexcept {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void *>(destroy_), &info) && info.dli_fname) {
            return info.dli_fname;
        }
        return "unknown plugin";
    }

    destroy_t destroy_;
    // Released after unique_ptr invokes the callback; the owner finalizes/unloads the plugin.
    std::shared_ptr<void> pluginOwner_;
};

using engine_ptr_t = std::unique_ptr<nixlBackendEngine, engineDeleter>;

// Every engine pointer names how its engine dies. Without this, a default-constructed
// deleter could adopt a plugin engine and operator delete it with the plugin unloaded.
static_assert(!std::is_default_constructible_v<engine_ptr_t>,
              "engine_ptr_t must carry an explicitly chosen destroy callback");

} // namespace nixl::fault

#endif
