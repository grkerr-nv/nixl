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

#include "backend/backend_plugin.h"

namespace {

nixlBackendEngine *
createEngine(const nixlBackendInitParams *) {
    return nullptr;
}

[[maybe_unused]] void
destroyEngine(nixlBackendEngine *) {}

const char *
getPluginName() {
    return "PLUGIN_LOADER_FIXTURE";
}

const char *
getPluginVersion() {
    return "0.0.1";
}

nixl_b_params_t
getBackendOptions() {
    return {};
}

nixl_mem_list_t
getBackendMems() {
    return {DRAM_SEG};
}

#ifdef NIXL_TEST_PLUGIN_BAD_API
constexpr int fixture_api_version = NIXL_PLUGIN_API_VERSION + 1;
#else
constexpr int fixture_api_version = NIXL_PLUGIN_API_VERSION;
#endif

#ifdef NIXL_TEST_PLUGIN_MISSING_CALLBACK
constexpr auto fixture_destroy_engine = static_cast<void (*)(nixlBackendEngine *)>(nullptr);
#else
constexpr auto fixture_destroy_engine = destroyEngine;
#endif

nixlBackendPlugin fixture_plugin = {fixture_api_version,
                                    createEngine,
                                    fixture_destroy_engine,
                                    getPluginName,
                                    getPluginVersion,
                                    getBackendOptions,
                                    getBackendMems};

} // namespace

extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return &fixture_plugin;
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
