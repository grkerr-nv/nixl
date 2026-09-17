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
#ifndef NIXL_TEST_FAULT_INJECTION_BACKEND_PLUGIN_LOADER_H
#define NIXL_TEST_FAULT_INJECTION_BACKEND_PLUGIN_LOADER_H

#include <filesystem>
#include <memory>
#include <string>

#include "backend/backend_engine.h"

/**
 * Loads a genuine backend plugin by absolute path.
 *
 * Engines created by the loader retain the plugin library until they are
 * destroyed, even if the loader itself is destroyed first.
 */
class nixlBackendPluginLoader {
    struct state;

public:
    class engineDeleter {
    public:
        void
        operator()(nixlBackendEngine *engine) const noexcept;

    private:
        friend class nixlBackendPluginLoader;

        explicit engineDeleter(std::shared_ptr<state> state);

        std::shared_ptr<state> state_;
    };

    using engine_ptr_t = std::unique_ptr<nixlBackendEngine, engineDeleter>;

    static std::unique_ptr<nixlBackendPluginLoader>
    load(const std::filesystem::path &path);

    engine_ptr_t
    createEngine(const nixlBackendInitParams *init_params) const;

    std::string
    getName() const;
    std::string
    getVersion() const;
    nixl_b_params_t
    getBackendOptions() const;
    nixl_mem_list_t
    getBackendMems() const;

private:
    explicit nixlBackendPluginLoader(std::shared_ptr<state> state);

    std::shared_ptr<state> state_;
};

#endif
