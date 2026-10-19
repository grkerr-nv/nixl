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

#include <memory>
#include <stdexcept>
#include <utility>

namespace nixl::fault {

// Plugin ownership keeps its callbacks valid through engine destruction.
class engineDeleter {
public:
    using destroy_t = void (*)(nixlBackendEngine *);

    engineDeleter() = default;

    engineDeleter(destroy_t destroy, std::shared_ptr<void> plugin_owner)
        : destroy_(destroy),
          pluginOwner_(std::move(plugin_owner)) {
        if (!destroy_ || !pluginOwner_) {
            throw std::invalid_argument("A plugin destroy callback and shared owner are required");
        }
    }

    void
    operator()(nixlBackendEngine *engine) const noexcept {
        // std::unique_ptr invokes this automatically during engine cleanup.
        if (engine) {
            destroy_(engine);
        }
    }

private:
    // Default cleanup for directly allocated engines; plugin engines use their callback.
    static void
    deleteEngine(nixlBackendEngine *engine) {
        delete engine;
    }

    destroy_t destroy_ = deleteEngine;
    // Released after unique_ptr invokes the callback; the owner finalizes/unloads the plugin.
    // Keep callbacks and backend code alive until the owning pointer is destroyed.
    std::shared_ptr<void> pluginOwner_;
};

using engine_ptr_t = std::unique_ptr<nixlBackendEngine, engineDeleter>;

} // namespace nixl::fault

#endif
