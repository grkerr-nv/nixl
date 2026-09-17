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

// One source, several plugins. Each NIXL_TEST_PLUGIN_* macro breaks the plugin
// in exactly one way so the loader can be held to one failure at a time; with
// no macro defined this builds a plugin that works.

#include "backend/backend_plugin.h"

namespace {

unsigned fini_count = 0;

// Just enough engine to be created and destroyed: the loader only cares that
// the plugin hands back something whose lifetime it has to respect.
class fixtureEngine : public nixlBackendEngine {
public:
    explicit fixtureEngine(const nixlBackendInitParams *init_params)
        : nixlBackendEngine(init_params) {}

    bool
    supportsRemote() const override {
        return false;
    }

    bool
    supportsLocal() const override {
        return true;
    }

    bool
    supportsNotif() const override {
        return false;
    }

    nixl_mem_list_t
    getSupportedMems() const override {
        return {DRAM_SEG};
    }

    nixl_status_t
    registerMem(const nixlBlobDesc &, const nixl_mem_t &, nixlBackendMD *&) override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    deregisterMem(nixlBackendMD *) override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    connect(const std::string &) override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    disconnect(const std::string &) override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    unloadMD(nixlBackendMD *) override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &,
             const nixl_meta_dlist_t &,
             const nixl_meta_dlist_t &,
             const std::string &,
             nixlBackendReqH *&,
             const nixl_opt_b_args_t *) const override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    postXfer(const nixl_xfer_op_t &,
             const nixl_meta_dlist_t &,
             const nixl_meta_dlist_t &,
             const std::string &,
             nixlBackendReqH *&,
             const nixl_opt_b_args_t *) const override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    checkXfer(nixlBackendReqH *) const override {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixl_status_t
    releaseReqH(nixlBackendReqH *) const override {
        return NIXL_ERR_NOT_SUPPORTED;
    }
};

nixlBackendEngine *
createEngine([[maybe_unused]] const nixlBackendInitParams *init_params) {
#ifdef NIXL_TEST_PLUGIN_NULL_ENGINE
    return nullptr;
#else
    return new fixtureEngine(init_params);
#endif
}

[[maybe_unused]] void
destroyEngine(nixlBackendEngine *engine) {
    delete engine;
}

const char *
getPluginName() {
    return "PLUGIN_LOADER_FIXTURE";
}

const char *
getPluginVersion() {
    return "0.0.1";
}

[[maybe_unused]] nixl_b_params_t
getBackendOptions() {
    return {};
}

[[maybe_unused]] nixl_mem_list_t
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

#ifdef NIXL_TEST_PLUGIN_NO_OPTIONAL
constexpr auto fixture_backend_options = static_cast<nixl_b_params_t (*)()>(nullptr);
constexpr auto fixture_backend_mems = static_cast<nixl_mem_list_t (*)()>(nullptr);
#else
constexpr auto fixture_backend_options = getBackendOptions;
constexpr auto fixture_backend_mems = getBackendMems;
#endif

[[maybe_unused]] nixlBackendPlugin fixture_plugin = {fixture_api_version,
                                                     createEngine,
                                                     fixture_destroy_engine,
                                                     getPluginName,
                                                     getPluginVersion,
                                                     fixture_backend_options,
                                                     fixture_backend_mems};

} // namespace

#ifndef NIXL_TEST_PLUGIN_NO_INIT
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
#ifdef NIXL_TEST_PLUGIN_NULL_INIT
    return nullptr;
#else
    return &fixture_plugin;
#endif
}
#endif

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {
    ++fini_count;
}

// Lets a test observe finalization without owning the loader's handle. The
// count survives dlclose because plugins are opened with RTLD_NODELETE.
extern "C" NIXL_PLUGIN_EXPORT unsigned
nixl_test_plugin_fini_count() {
    return fini_count;
}
