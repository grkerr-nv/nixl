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
#ifndef NIXL_FAULT_INJECTION_FORWARDING_ENGINE_H
#define NIXL_FAULT_INJECTION_FORWARDING_ENGINE_H

#include "backend/backend_engine.h"

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
    operator()(nixlBackendEngine *engine) const {
        destroy_(engine);
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

    destroy_t destroy_;
    // Released after unique_ptr invokes the callback; the owner finalizes/unloads the plugin.
    std::shared_ptr<void> pluginOwner_;
};

using engine_ptr_t = std::unique_ptr<nixlBackendEngine, engineDeleter>;

// Every engine pointer names how its engine dies. Without this, a default-constructed
// deleter could adopt a plugin engine and operator delete it with the plugin unloaded.
static_assert(!std::is_default_constructible_v<engine_ptr_t>,
              "engine_ptr_t must carry an explicitly chosen destroy callback");

// Owns an initialized engine; opaque handles remain owned by that engine.
// Base-class getters expose a snapshot of initialization state. The nonvirtual
// telemetry getter reads this wrapper's own queue, which stays empty; core does
// not currently drain backend telemetry queues, so no events are lost.
class forwardingEngine : public nixlBackendEngine {
public:
    forwardingEngine(const nixlBackendInitParams *params, engine_ptr_t engine)
        : nixlBackendEngine(validateParams(params)),
          engine_(std::move(engine)) {
        if (!engine_) {
            throw std::invalid_argument("A wrapped backend engine is required");
        }
        if (getType() != engine_->getType()) {
            throw std::invalid_argument(
                "Wrapped backend type does not match initialization parameters");
        }
        initErr = engine_->getInitErr();
        // Adopt engine-discovered keys, preserve caller-only keys, and reject conflicts.
        for (const auto &[key, value] : engine_->getCustomParams()) {
            static_cast<void>(setInitParam(key, value));
            if (getCustomParams().at(key) != value) {
                throw std::invalid_argument(
                    "Wrapped backend parameters do not match initialization parameters");
            }
        }
    }

    bool
    supportsRemote() const override {
        return engine_->supportsRemote();
    }

    bool
    supportsLocal() const override {
        return engine_->supportsLocal();
    }

    bool
    supportsNotif() const override {
        return engine_->supportsNotif();
    }

    [[nodiscard]] bool
    supportsTraceContext() const noexcept override {
        return engine_->supportsTraceContext();
    }

    nixl_mem_list_t
    getSupportedMems() const override {
        return engine_->getSupportedMems();
    }

    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &type, nixlBackendMD *&out) override {
        return engine_->registerMem(mem, type, out);
    }

    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override {
        return engine_->deregisterMem(meta);
    }

    nixl_status_t
    connect(const std::string &peer) override {
        return engine_->connect(peer);
    }

    nixl_status_t
    disconnect(const std::string &peer) override {
        return engine_->disconnect(peer);
    }

    nixl_status_t
    unloadMD(nixlBackendMD *meta) override {
        return engine_->unloadMD(meta);
    }

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &peer,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *args = nullptr) const override {
        return engine_->prepXfer(operation, local, remote, peer, handle, args);
    }

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &peer,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *args = nullptr) const override {
        return engine_->postXfer(operation, local, remote, peer, handle, args);
    }

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override {
        return engine_->checkXfer(handle);
    }

    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override {
        return engine_->releaseReqH(handle);
    }

    nixl_status_t
    prepMemView(const nixl_remote_meta_dlist_t &descs,
                nixlMemViewH &handle,
                const nixl_opt_b_args_t *args = nullptr) const override {
        return engine_->prepMemView(descs, handle, args);
    }

    nixl_status_t
    prepMemView(const nixl_meta_dlist_t &descs,
                nixlMemViewH &handle,
                const nixl_opt_b_args_t *args = nullptr) const override {
        return engine_->prepMemView(descs, handle, args);
    }

    void
    releaseMemView(nixlMemViewH handle) const override {
        engine_->releaseMemView(handle);
    }

    nixl_status_t
    getPublicData(const nixlBackendMD *meta, std::string &data) const override {
        return engine_->getPublicData(meta, data);
    }

    nixl_status_t
    getConnInfo(std::string &data) const override {
        return engine_->getConnInfo(data);
    }

    nixl_status_t
    loadRemoteConnInfo(const std::string &peer, const std::string &data) override {
        return engine_->loadRemoteConnInfo(peer, data);
    }

    nixl_status_t
    loadRemoteMD(const nixlBlobDesc &desc,
                 const nixl_mem_t &type,
                 const std::string &peer,
                 nixlBackendMD *&out) override {
        return engine_->loadRemoteMD(desc, type, peer, out);
    }

    nixl_status_t
    loadLocalMD(nixlBackendMD *meta, nixlBackendMD *&out) override {
        return engine_->loadLocalMD(meta, out);
    }

    nixl_status_t
    getNotifs(notif_list_t &notifs) override {
        return engine_->getNotifs(notifs);
    }

    nixl_status_t
    genNotif(const std::string &peer, const std::string &message) const override {
        return engine_->genNotif(peer, message);
    }

    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs,
             std::vector<nixl_query_resp_t> &responses) const override {
        return engine_->queryMem(descs, responses);
    }

    nixl_status_t
    estimateXferCost(const nixl_xfer_op_t &operation,
                     const nixl_meta_dlist_t &local,
                     const nixl_meta_dlist_t &remote,
                     const std::string &peer,
                     nixlBackendReqH *const &handle,
                     std::chrono::microseconds &duration,
                     std::chrono::microseconds &margin,
                     nixl_cost_t &method,
                     const nixl_opt_args_t *args = nullptr) const override {
        return engine_->estimateXferCost(
            operation, local, remote, peer, handle, duration, margin, method, args);
    }

private:
    // Validate before the base constructor dereferences params; a null parameter map is valid.
    static const nixlBackendInitParams *
    validateParams(const nixlBackendInitParams *params) {
        if (!params) {
            throw std::invalid_argument("Backend initialization parameters are required");
        }
        return params;
    }

    // Own the genuine engine and invoke its deleter on destruction or constructor failure.
    engine_ptr_t engine_;
};

} // namespace nixl::fault

#endif
