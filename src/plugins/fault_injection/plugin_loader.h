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

#ifndef NIXL_SRC_PLUGINS_FAULT_INJECTION_PLUGIN_LOADER_H
#define NIXL_SRC_PLUGINS_FAULT_INJECTION_PLUGIN_LOADER_H

#include "engine_ptr.h"

#include <filesystem>
#include <memory>
#include <string>

namespace nixl::fault {

// Loads a genuine backend by path, independently of NIXL's name-based discovery.
class pluginLoader {
    struct pluginState;

public:
    using engineDeleter = nixl::fault::engineDeleter;
    using engine_ptr_t = nixl::fault::engine_ptr_t;

    // Throws for relative paths, loading errors, or an invalid plugin interface.
    // deepbind adds RTLD_DEEPBIND, as core does for UCX when NIXL_UCX_DEEPBIND is set.
    explicit pluginLoader(const std::filesystem::path &path, bool deepbind = false);

    pluginLoader(const pluginLoader &) = delete;
    pluginLoader &
    operator=(const pluginLoader &) = delete;

    std::string
    getName() const;
    std::string
    getVersion() const;
    nixl_b_params_t
    getBackendOptions() const;
    nixl_mem_list_t
    getBackendMems() const;

    // Pass params unchanged. Throws if params is null or engine creation fails.
    // Engines may outlive this loader; do not release them from their owning pointer.
    engine_ptr_t
    createEngine(const nixlBackendInitParams *params) const;

private:
    // Shared by this loader and engine deleters. Finalize only after the last owner.
    std::shared_ptr<pluginState> state_;
};

} // namespace nixl::fault

#endif
