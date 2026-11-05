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

#include "engine_ptr.h"
#include "fault_runtime.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace nixl::fault {

// Owns an initialized engine; opaque handles remain owned by that engine.
// Base-class getters expose a snapshot of initialization state. The nonvirtual
// telemetry getter reads this wrapper's own queue, which stays empty; core does
// not currently drain backend telemetry queues, so no events are lost.
class forwardingEngine : public nixlBackendEngine {
public:
    forwardingEngine(const nixlBackendInitParams *params,
                     engine_ptr_t engine,
                     std::shared_ptr<faultRuntime> runtime = nullptr)
        : nixlBackendEngine(validateParams(params)),
          runtime_(std::move(runtime)),
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
        if (const auto status = injectedStatus(backend_method_t::REGISTER_MEM, std::nullopt)) {
            return *status;
        }
        return engine_->registerMem(mem, type, out);
    }

    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override {
        if (const auto status = injectedStatus(backend_method_t::DEREGISTER_MEM, std::nullopt)) {
            // An error means the call failed, not that it never ran, so release the resource
            // anyway. A non-error status is the other fault: cleanup claimed but not done.
            if (isErrorStatus(*status)) {
                engine_->deregisterMem(meta);
            }
            return *status;
        }
        return engine_->deregisterMem(meta);
    }

    nixl_status_t
    connect(const std::string &peer) override {
        if (const auto status = injectedStatus(backend_method_t::CONNECT, std::nullopt)) {
            return *status;
        }
        return engine_->connect(peer);
    }

    nixl_status_t
    disconnect(const std::string &peer) override {
        if (const auto status = injectedStatus(backend_method_t::DISCONNECT, std::nullopt)) {
            // An error means the call failed, not that it never ran, so release the resource
            // anyway. A non-error status is the other fault: cleanup claimed but not done.
            if (isErrorStatus(*status)) {
                engine_->disconnect(peer);
            }
            return *status;
        }
        return engine_->disconnect(peer);
    }

    nixl_status_t
    unloadMD(nixlBackendMD *meta) override {
        if (const auto status = injectedStatus(backend_method_t::UNLOAD_MD, std::nullopt)) {
            // An error means the call failed, not that it never ran, so release the resource
            // anyway. A non-error status is the other fault: cleanup claimed but not done.
            if (isErrorStatus(*status)) {
                engine_->unloadMD(meta);
            }
            return *status;
        }
        return engine_->unloadMD(meta);
    }

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &peer,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *args = nullptr) const override {
        if (const auto status = injectedStatus(backend_method_t::PREP_XFER, operation)) {
            return *status;
        }
        return engine_->prepXfer(operation, local, remote, peer, handle, args);
    }

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &peer,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *args = nullptr) const override {
        if (const auto status = injectedStatus(backend_method_t::POST_XFER, operation)) {
            return *status;
        }
        return engine_->postXfer(operation, local, remote, peer, handle, args);
    }

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override {
        if (const auto status = injectedStatus(backend_method_t::CHECK_XFER, std::nullopt)) {
            return *status;
        }
        return engine_->checkXfer(handle);
    }

    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override {
        if (const auto status = injectedStatus(backend_method_t::RELEASE_REQ_H, std::nullopt)) {
            // An error means the call failed, not that it never ran, so release the resource
            // anyway. A non-error status is the other fault: cleanup claimed but not done.
            if (isErrorStatus(*status)) {
                engine_->releaseReqH(handle);
            }
            return *status;
        }
        return engine_->releaseReqH(handle);
    }

    nixl_status_t
    prepMemView(const nixl_remote_meta_dlist_t &descs,
                nixlMemViewH &handle,
                const nixl_opt_b_args_t *args = nullptr) const override {
        if (const auto status = injectedStatus(backend_method_t::PREP_MEM_VIEW, std::nullopt)) {
            return *status;
        }
        return engine_->prepMemView(descs, handle, args);
    }

    nixl_status_t
    prepMemView(const nixl_meta_dlist_t &descs,
                nixlMemViewH &handle,
                const nixl_opt_b_args_t *args = nullptr) const override {
        if (const auto status = injectedStatus(backend_method_t::PREP_MEM_VIEW, std::nullopt)) {
            return *status;
        }
        return engine_->prepMemView(descs, handle, args);
    }

    void
    releaseMemView(nixlMemViewH handle) const override {
        engine_->releaseMemView(handle);
    }

    nixl_status_t
    getPublicData(const nixlBackendMD *meta, std::string &data) const override {
        if (const auto status = injectedStatus(backend_method_t::GET_PUBLIC_DATA, std::nullopt)) {
            return *status;
        }
        return engine_->getPublicData(meta, data);
    }

    nixl_status_t
    getConnInfo(std::string &data) const override {
        if (const auto status = injectedStatus(backend_method_t::GET_CONN_INFO, std::nullopt)) {
            return *status;
        }
        return engine_->getConnInfo(data);
    }

    nixl_status_t
    loadRemoteConnInfo(const std::string &peer, const std::string &data) override {
        if (const auto status =
                injectedStatus(backend_method_t::LOAD_REMOTE_CONN_INFO, std::nullopt)) {
            return *status;
        }
        return engine_->loadRemoteConnInfo(peer, data);
    }

    nixl_status_t
    loadRemoteMD(const nixlBlobDesc &desc,
                 const nixl_mem_t &type,
                 const std::string &peer,
                 nixlBackendMD *&out) override {
        if (const auto status = injectedStatus(backend_method_t::LOAD_REMOTE_MD, std::nullopt)) {
            return *status;
        }
        return engine_->loadRemoteMD(desc, type, peer, out);
    }

    nixl_status_t
    loadLocalMD(nixlBackendMD *meta, nixlBackendMD *&out) override {
        if (const auto status = injectedStatus(backend_method_t::LOAD_LOCAL_MD, std::nullopt)) {
            return *status;
        }
        return engine_->loadLocalMD(meta, out);
    }

    nixl_status_t
    getNotifs(notif_list_t &notifs) override {
        if (const auto status = injectedStatus(backend_method_t::GET_NOTIFS, std::nullopt)) {
            return *status;
        }
        return engine_->getNotifs(notifs);
    }

    nixl_status_t
    genNotif(const std::string &peer, const std::string &message) const override {
        if (const auto status = injectedStatus(backend_method_t::GEN_NOTIF, std::nullopt)) {
            return *status;
        }
        return engine_->genNotif(peer, message);
    }

    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs,
             std::vector<nixl_query_resp_t> &responses) const override {
        if (const auto status = injectedStatus(backend_method_t::QUERY_MEM, std::nullopt)) {
            return *status;
        }
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
        if (const auto status = injectedStatus(backend_method_t::ESTIMATE_XFER_COST, operation)) {
            return *status;
        }
        return engine_->estimateXferCost(
            operation, local, remote, peer, handle, duration, margin, method, args);
    }

private:
    std::optional<nixl_status_t>
    injectedStatus(backend_method_t method, std::optional<nixl_xfer_op_t> operation) const {
        return runtime_ ? runtime_->evaluate(method, operation) : std::nullopt;
    }

    // Validate before the base constructor dereferences params; a null parameter map is valid.
    static const nixlBackendInitParams *
    validateParams(const nixlBackendInitParams *params) {
        if (!params) {
            throw std::invalid_argument("Backend initialization parameters are required");
        }
        return params;
    }

    std::shared_ptr<faultRuntime> runtime_;
    // Own the genuine engine and invoke its deleter on destruction or constructor failure.
    engine_ptr_t engine_;
};

} // namespace nixl::fault

#endif
