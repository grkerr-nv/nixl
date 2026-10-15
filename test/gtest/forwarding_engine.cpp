/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#include "../../src/plugins/fault_injection/forwarding_engine.h"
#include "mocks/gmock_engine.h"

using testing::_;
using testing::Ref;
using testing::Return;
using nixl::fault::EngineDeleter;
using nixl::fault::EnginePtr;
using nixl::fault::ForwardingEngine;

namespace {

// Extend the existing mock for optional methods it does not yet implement.
class ForwardingMock : public mocks::GMockBackendEngine {
public:
    explicit ForwardingMock(const nixlBackendInitParams *params)
        : mocks::GMockBackendEngine(params) {}

    ~ForwardingMock() override {
        if (destructions) {
            ++*destructions;
        }
    }

    using mocks::GMockBackendEngine::prepMemView;
    using mocks::GMockBackendEngine::gmock_prepMemView;
    MOCK_METHOD(nixl_status_t,
                prepMemView,
                (const nixl_meta_dlist_t &, nixlMemViewH &, const nixl_opt_b_args_t *),
                (const, override));
    MOCK_METHOD(void, releaseMemView, (nixlMemViewH), (const, override));
    MOCK_METHOD(nixl_status_t,
                queryMem,
                (const nixl_reg_dlist_t &, std::vector<nixl_query_resp_t> &),
                (const, override));
    MOCK_METHOD(nixl_status_t,
                estimateXferCost,
                (const nixl_xfer_op_t &,
                 const nixl_meta_dlist_t &,
                 const nixl_meta_dlist_t &,
                 const std::string &,
                 nixlBackendReqH *const &,
                 std::chrono::microseconds &,
                 std::chrono::microseconds &,
                 nixl_cost_t &,
                 const nixl_opt_args_t *),
                (const, override));

    void
    failInitialization() {
        initErr = true;
    }

    void
    addParameter(const std::string &key, const std::string &value) {
        ASSERT_EQ(setInitParam(key, value), NIXL_SUCCESS);
    }

    int *destructions = nullptr;
    int *destroyCallbacks = nullptr;
};

class ForwardingEngineTest : public testing::Test {
protected:
    nixl_b_params_t custom{{"caller", "value"}};
    nixlBackendInitParams params{.localAgent = "local",
                                 .type = "UCX",
                                 .customParams = &custom,
                                 .syncMode = nixl_thread_sync_t::NIXL_THREAD_SYNC_NONE};
    using Mock = testing::StrictMock<ForwardingMock>;

    EnginePtr
    makeEngine(int *destructions = nullptr) {
        auto *mock = new Mock(&params);
        mock->destructions = destructions;
        return EnginePtr(mock);
    }
};

TEST_F(ForwardingEngineTest, PreservesInitializationStateThroughBasePointer) {
    auto engine = makeEngine();
    auto *mock = static_cast<Mock *>(engine.get());
    mock->failInitialization();
    mock->addParameter("backend", "discovered-value");
    ForwardingEngine wrapper(&params, std::move(engine));
    nixlBackendEngine &base = wrapper;
    EXPECT_TRUE(base.getInitErr());
    EXPECT_EQ(base.getType(), "UCX");
    EXPECT_EQ(base.getCustomParams(),
              (nixl_b_params_t{{"caller", "value"}, {"backend", "discovered-value"}}));
    EXPECT_TRUE(base.getTelemetryEvents().empty());
    EXPECT_EQ(custom.size(), 1U);
}

TEST_F(ForwardingEngineTest, ForwardsCapabilities) {
    auto engine = makeEngine();
    auto *mock = static_cast<Mock *>(engine.get());
    EXPECT_CALL(*mock, supportsRemote()).WillOnce(Return(false));
    EXPECT_CALL(*mock, supportsLocal()).WillOnce(Return(true));
    EXPECT_CALL(*mock, supportsNotif()).WillOnce(Return(false));
    EXPECT_CALL(*mock, getSupportedMems()).WillOnce(Return(nixl_mem_list_t{DRAM_SEG, VRAM_SEG}));
    ForwardingEngine wrapper(&params, std::move(engine));
    nixlBackendEngine &base = wrapper;
    EXPECT_FALSE(base.supportsRemote());
    EXPECT_TRUE(base.supportsLocal());
    EXPECT_FALSE(base.supportsNotif());
    EXPECT_EQ(base.getSupportedMems(), (nixl_mem_list_t{DRAM_SEG, VRAM_SEG}));
}

TEST_F(ForwardingEngineTest, DestroysOwnedEngineExactlyOnce) {
    int destructions = 0;
    auto engine = makeEngine(&destructions);
    {
        std::unique_ptr<nixlBackendEngine> wrapper =
            std::make_unique<ForwardingEngine>(&params, std::move(engine));
        EXPECT_EQ(destructions, 0);
    }
    EXPECT_EQ(destructions, 1);
}

TEST_F(ForwardingEngineTest, UsesPluginDestroyCallbackExactlyOnce) {
    int destructions = 0;
    int callbacks = 0;
    auto *mock = new Mock(&params);
    mock->destructions = &destructions;
    mock->destroyCallbacks = &callbacks;
    EnginePtr engine(mock, EngineDeleter([](nixlBackendEngine *engine) {
                         ++*static_cast<ForwardingMock *>(engine)->destroyCallbacks;
                         delete engine;
                     }));
    { ForwardingEngine wrapper(&params, std::move(engine)); }
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(destructions, 1);
}

TEST_F(ForwardingEngineTest, RejectsNullEngineAndDestroyCallback) {
    EXPECT_THROW(ForwardingEngine(&params, EnginePtr{}), std::invalid_argument);
    EXPECT_THROW(EngineDeleter(nullptr), std::invalid_argument);
}

TEST_F(ForwardingEngineTest, RejectsInvalidInitializationParametersAndCleansUp) {
    int destructions = 0;
    auto engine = makeEngine(&destructions);
    EXPECT_THROW(ForwardingEngine(nullptr, std::move(engine)), std::invalid_argument);
    EXPECT_EQ(destructions, 1);
    auto engine2 = makeEngine(&destructions);
    params.customParams = nullptr;
    EXPECT_THROW(ForwardingEngine(&params, std::move(engine2)), std::invalid_argument);
    EXPECT_EQ(destructions, 2);
}

TEST_F(ForwardingEngineTest, AcceptsTelemetryEnabledInitialization) {
    params.enableTelemetry_ = true;
    ForwardingEngine wrapper(&params, makeEngine());
    nixlBackendEngine &base = wrapper;
    EXPECT_FALSE(base.getInitErr());
    EXPECT_TRUE(base.getTelemetryEvents().empty());
}

TEST_F(ForwardingEngineTest, RejectsMismatchedBackendIdentityAndCleansUp) {
    int destructions = 0;
    auto engine = makeEngine(&destructions);
    params.type = "POSIX";
    EXPECT_THROW(ForwardingEngine(&params, std::move(engine)), std::invalid_argument);
    EXPECT_EQ(destructions, 1);
}

TEST_F(ForwardingEngineTest, RejectsMismatchedBackendParametersAndCleansUp) {
    int destructions = 0;
    auto engine = makeEngine(&destructions);
    custom["caller"] = "changed-after-engine-creation";
    EXPECT_THROW(ForwardingEngine(&params, std::move(engine)), std::invalid_argument);
    EXPECT_EQ(destructions, 1);
}

class ForwardingStatusTest : public ForwardingEngineTest,
                             public testing::WithParamInterface<nixl_status_t> {};

TEST_P(ForwardingStatusTest, PreservesRegistrationMetadataAndConnectionResults) {
    const auto status = GetParam();
    auto engine = makeEngine();
    auto *mock = static_cast<Mock *>(engine.get());
    nixlBlobDesc desc(0x1234, 256, 7);
    const nixl_mem_t memory = DRAM_SEG;
    nixlBackendMD metadata(false);
    nixlBackendMD *output = nullptr;
    const std::string peer = "peer";
    const std::string connection = "connection";
    std::string publicData;
    std::string connInfo;
    EXPECT_CALL(*mock, registerMem(Ref(desc), Ref(memory), Ref(output)))
        .WillOnce([&](const auto &, const auto &, auto &out) {
            out = &metadata;
            return status;
        });
    EXPECT_CALL(*mock, deregisterMem(&metadata)).WillOnce(Return(status));
    EXPECT_CALL(*mock, connect(Ref(peer))).WillOnce(Return(status));
    EXPECT_CALL(*mock, disconnect(Ref(peer))).WillOnce(Return(status));
    EXPECT_CALL(*mock, unloadMD(&metadata)).WillOnce(Return(status));
    EXPECT_CALL(*mock, getPublicData(&metadata, Ref(publicData))).WillOnce([&](auto *, auto &data) {
        data = "public-data";
        return status;
    });
    EXPECT_CALL(*mock, getConnInfo(Ref(connInfo))).WillOnce([&](auto &data) {
        data = connection;
        return status;
    });
    EXPECT_CALL(*mock, loadRemoteConnInfo(Ref(peer), Ref(connection))).WillOnce(Return(status));
    EXPECT_CALL(*mock, loadRemoteMD(Ref(desc), Ref(memory), Ref(peer), Ref(output)))
        .WillOnce([&](const auto &, const auto &, const auto &, auto &out) {
            out = &metadata;
            return status;
        });
    EXPECT_CALL(*mock, loadLocalMD(&metadata, Ref(output))).WillOnce([&](auto *, auto &out) {
        out = &metadata;
        return status;
    });
    ForwardingEngine wrapper(&params, std::move(engine));
    nixlBackendEngine &base = wrapper;
    EXPECT_EQ(base.registerMem(desc, memory, output), status);
    EXPECT_EQ(output, &metadata);
    EXPECT_EQ(base.deregisterMem(output), status);
    EXPECT_EQ(base.connect(peer), status);
    EXPECT_EQ(base.disconnect(peer), status);
    EXPECT_EQ(base.unloadMD(output), status);
    EXPECT_EQ(base.getPublicData(output, publicData), status);
    EXPECT_EQ(publicData, "public-data");
    EXPECT_EQ(base.getConnInfo(connInfo), status);
    EXPECT_EQ(connInfo, connection);
    EXPECT_EQ(base.loadRemoteConnInfo(peer, connection), status);
    output = nullptr;
    EXPECT_EQ(base.loadRemoteMD(desc, memory, peer, output), status);
    EXPECT_EQ(output, &metadata);
    output = nullptr;
    EXPECT_EQ(base.loadLocalMD(&metadata, output), status);
    EXPECT_EQ(output, &metadata);
}

TEST_P(ForwardingStatusTest, PreservesTransferHandlesAndOptionalArguments) {
    const auto status = GetParam();
    auto engine = makeEngine();
    auto *mock = static_cast<Mock *>(engine.get());
    const nixl_xfer_op_t operation = NIXL_WRITE;
    const nixl_meta_dlist_t local(DRAM_SEG);
    const nixl_meta_dlist_t remote(VRAM_SEG);
    const std::string peer = "peer";
    nixl_opt_b_args_t args{.notifMsg = "notification", .hasNotif = true, .customParam = "custom"};
    nixlBackendReqH prepared;
    nixlBackendReqH posted;
    nixlBackendReqH *handle = nullptr;
    EXPECT_CALL(*mock,
                prepXfer(Ref(operation), Ref(local), Ref(remote), Ref(peer), Ref(handle), &args))
        .WillOnce([&](const auto &, const auto &, const auto &, const auto &, auto &out, auto *) {
            EXPECT_EQ(out, nullptr);
            out = &prepared;
            return status;
        });
    EXPECT_CALL(*mock,
                postXfer(Ref(operation), Ref(local), Ref(remote), Ref(peer), Ref(handle), &args))
        .WillOnce([&](const auto &, const auto &, const auto &, const auto &, auto &out, auto *) {
            EXPECT_EQ(out, &prepared);
            out = &posted;
            return status;
        });
    EXPECT_CALL(*mock, checkXfer(&posted)).WillOnce(Return(status));
    EXPECT_CALL(*mock, releaseReqH(&posted)).WillOnce(Return(status));
    ForwardingEngine wrapper(&params, std::move(engine));
    const nixlBackendEngine &base = wrapper;
    EXPECT_EQ(base.prepXfer(operation, local, remote, peer, handle, &args), status);
    EXPECT_EQ(handle, &prepared);
    EXPECT_EQ(base.postXfer(operation, local, remote, peer, handle, &args), status);
    EXPECT_EQ(handle, &posted);
    EXPECT_EQ(base.checkXfer(handle), status);
    EXPECT_EQ(base.releaseReqH(handle), status);
}

TEST_P(ForwardingStatusTest, PreservesOptionalMethodOutputs) {
    const auto status = GetParam();
    auto engine = makeEngine();
    auto *mock = static_cast<Mock *>(engine.get());
    const nixl_meta_dlist_t local(DRAM_SEG);
    const nixl_remote_meta_dlist_t remote(DRAM_SEG);
    const nixl_reg_dlist_t registration(DRAM_SEG);
    nixl_opt_b_args_t args;
    int localView = 0, remoteView = 0;
    nixlMemViewH view = nullptr;
    notif_list_t notifications;
    const std::string peer = "peer", message = "message";
    std::vector<nixl_query_resp_t> responses;
    const nixl_xfer_op_t operation = NIXL_READ;
    nixlBackendReqH request;
    nixlBackendReqH *const handle = &request;
    const nixl_meta_dlist_t remoteTransfer(DRAM_SEG);
    std::chrono::microseconds duration{0}, margin{0};
    nixl_cost_t method = nixl_cost_t::ANALYTICAL_BACKEND;
    nixl_opt_args_t costArgs;
    EXPECT_CALL(
        *mock,
        prepMemView(testing::Matcher<const nixl_meta_dlist_t &>(Ref(local)), Ref(view), &args))
        .WillOnce([&](const nixl_meta_dlist_t &, auto &out, auto *) {
            out = &localView;
            return status;
        });
    EXPECT_CALL(*mock,
                prepMemView(testing::Matcher<const nixl_remote_meta_dlist_t &>(Ref(remote)),
                            Ref(view),
                            &args))
        .WillOnce([&](const nixl_remote_meta_dlist_t &, auto &out, auto *) {
            out = &remoteView;
            return status;
        });
    EXPECT_CALL(*mock, releaseMemView(&localView));
    EXPECT_CALL(*mock, releaseMemView(&remoteView));
    EXPECT_CALL(*mock, getNotifs(Ref(notifications))).WillOnce([&](auto &out) {
        out.emplace_back(peer, message);
        return status;
    });
    EXPECT_CALL(*mock, genNotif(Ref(peer), Ref(message))).WillOnce(Return(status));
    EXPECT_CALL(*mock, queryMem(Ref(registration), Ref(responses)))
        .WillOnce([&](const auto &, auto &out) {
            out.emplace_back(nixl_b_params_t{{"size", "4096"}});
            return status;
        });
    EXPECT_CALL(*mock,
                estimateXferCost(Ref(operation),
                                 Ref(local),
                                 Ref(remoteTransfer),
                                 Ref(peer),
                                 Ref(handle),
                                 Ref(duration),
                                 Ref(margin),
                                 Ref(method),
                                 &costArgs))
        .WillOnce([&](const auto &,
                      const auto &,
                      const auto &,
                      const auto &,
                      auto &,
                      auto &outDuration,
                      auto &outMargin,
                      auto &outMethod,
                      auto *) {
            outDuration = std::chrono::microseconds{123};
            outMargin = std::chrono::microseconds{7};
            outMethod = nixl_cost_t::ANALYTICAL_BACKEND;
            return status;
        });
    ForwardingEngine wrapper(&params, std::move(engine));
    nixlBackendEngine &base = wrapper;
    EXPECT_EQ(base.prepMemView(local, view, &args), status);
    EXPECT_EQ(view, &localView);
    base.releaseMemView(view);
    EXPECT_EQ(base.prepMemView(remote, view, &args), status);
    EXPECT_EQ(view, &remoteView);
    base.releaseMemView(view);
    EXPECT_EQ(base.getNotifs(notifications), status);
    EXPECT_EQ(notifications, (notif_list_t{{peer, message}}));
    EXPECT_EQ(base.genNotif(peer, message), status);
    EXPECT_EQ(base.queryMem(registration, responses), status);
    ASSERT_EQ(responses.size(), 1U);
    EXPECT_EQ(responses.front(), (nixl_b_params_t{{"size", "4096"}}));
    EXPECT_EQ(
        base.estimateXferCost(
            operation, local, remoteTransfer, peer, handle, duration, margin, method, &costArgs),
        status);
    EXPECT_EQ(duration.count(), 123);
    EXPECT_EQ(margin.count(), 7);
    EXPECT_EQ(method, nixl_cost_t::ANALYTICAL_BACKEND);
}

INSTANTIATE_TEST_SUITE_P(AllStatuses,
                         ForwardingStatusTest,
                         testing::Values(NIXL_SUCCESS,
                                         NIXL_IN_PROG,
                                         NIXL_ERR_NOT_POSTED,
                                         NIXL_ERR_INVALID_PARAM,
                                         NIXL_ERR_BACKEND,
                                         NIXL_ERR_NOT_FOUND,
                                         NIXL_ERR_MISMATCH,
                                         NIXL_ERR_NOT_ALLOWED,
                                         NIXL_ERR_REPOST_ACTIVE,
                                         NIXL_ERR_UNKNOWN,
                                         NIXL_ERR_NOT_SUPPORTED,
                                         NIXL_ERR_REMOTE_DISCONNECT,
                                         NIXL_ERR_CANCELED,
                                         NIXL_ERR_NO_TELEMETRY));

} // namespace
