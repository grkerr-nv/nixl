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
#include "../../src/plugins/fault_injection/forwarding_engine.h"
#include "mocks/gmock_engine.h"

using testing::AllOf;
using testing::DoAll;
using testing::Eq;
using testing::Ref;
using testing::Return;
using testing::SetArgReferee;
using nixl::fault::EngineDeleter;
using nixl::fault::EnginePtr;
using nixl::fault::ForwardingEngine;

namespace {

// Add state and lifetime controls specific to the forwarding tests.
class ForwardingMock : public mocks::GMockBackendEngine {
public:
    using mocks::GMockBackendEngine::GMockBackendEngine;
    using nixlBackendEngine::setInitParam;

    ~ForwardingMock() override {
        if (destructions) {
            ++*destructions;
        }
    }

    void
    failInitialization() {
        initErr = true;
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

    std::unique_ptr<Mock, EngineDeleter>
    makeEngine(int *destructions = nullptr) {
        auto *mock = new Mock(&params);
        mock->destructions = destructions;
        return std::unique_ptr<Mock, EngineDeleter>(mock);
    }
};

TEST_F(ForwardingEngineTest, PreservesInitializationStateThroughBasePointer) {
    auto engine = makeEngine();
    engine->failInitialization();
    ASSERT_EQ(engine->setInitParam("backend", "discovered-value"), NIXL_SUCCESS);
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
    EXPECT_CALL(*engine, supportsRemote()).WillOnce(Return(false));
    EXPECT_CALL(*engine, supportsLocal()).WillOnce(Return(true));
    EXPECT_CALL(*engine, supportsNotif()).WillOnce(Return(false));
    EXPECT_CALL(*engine, supportsTraceContext()).WillOnce(Return(true)).WillOnce(Return(false));
    EXPECT_CALL(*engine, getSupportedMems()).WillOnce(Return(nixl_mem_list_t{DRAM_SEG, VRAM_SEG}));
    ForwardingEngine wrapper(&params, std::move(engine));
    nixlBackendEngine &base = wrapper;
    EXPECT_FALSE(base.supportsRemote());
    EXPECT_TRUE(base.supportsLocal());
    EXPECT_FALSE(base.supportsNotif());
    EXPECT_TRUE(base.supportsTraceContext());
    EXPECT_FALSE(base.supportsTraceContext());
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
}

TEST_F(ForwardingEngineTest, AcceptsNullParameterMapAndTelemetry) {
    params.customParams = nullptr;
    params.enableTelemetry_ = true;
    ForwardingEngine wrapper(&params, makeEngine());
    nixlBackendEngine &base = wrapper;
    EXPECT_FALSE(base.getInitErr());
    EXPECT_EQ(base.getType(), params.type);
    EXPECT_TRUE(base.getCustomParams().empty());
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

// Cover every forwarding route once, using representative error, in-progress, and
// success results across these groups. Check arguments, outputs, and handles too.
TEST_F(ForwardingEngineTest, PreservesRegistrationMetadataAndConnectionResults) {
    const auto status = NIXL_ERR_BACKEND;
    auto engine = makeEngine();
    nixlBlobDesc desc(0x1234, 256, 7);
    const nixl_mem_t memory = DRAM_SEG;
    nixlBackendMD metadata(false);
    nixlBackendMD *output = nullptr;
    const std::string peer = "peer";
    const std::string connection = "connection";
    std::string publicData;
    std::string connInfo;
    EXPECT_CALL(*engine, registerMem(Ref(desc), Ref(memory), Ref(output)))
        .WillOnce(DoAll(SetArgReferee<2>(&metadata), Return(status)));
    EXPECT_CALL(*engine, deregisterMem(&metadata)).WillOnce(Return(status));
    EXPECT_CALL(*engine, connect(Ref(peer))).WillOnce(Return(status));
    EXPECT_CALL(*engine, disconnect(Ref(peer))).WillOnce(Return(status));
    EXPECT_CALL(*engine, unloadMD(&metadata)).WillOnce(Return(status));
    EXPECT_CALL(*engine, getPublicData(&metadata, Ref(publicData)))
        .WillOnce(DoAll(SetArgReferee<1>("public-data"), Return(status)));
    EXPECT_CALL(*engine, getConnInfo(Ref(connInfo)))
        .WillOnce(DoAll(SetArgReferee<0>(connection), Return(status)));
    EXPECT_CALL(*engine, loadRemoteConnInfo(Ref(peer), Ref(connection))).WillOnce(Return(status));
    EXPECT_CALL(*engine, loadRemoteMD(Ref(desc), Ref(memory), Ref(peer), Ref(output)))
        .WillOnce(DoAll(SetArgReferee<3>(&metadata), Return(status)));
    EXPECT_CALL(*engine, loadLocalMD(&metadata, Ref(output)))
        .WillOnce(DoAll(SetArgReferee<1>(&metadata), Return(status)));
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

TEST_F(ForwardingEngineTest, PreservesTransferHandlesAndOptionalArguments) {
    const auto status = NIXL_IN_PROG;
    auto engine = makeEngine();
    const nixl_xfer_op_t operation = NIXL_WRITE;
    const nixl_meta_dlist_t local(DRAM_SEG);
    const nixl_meta_dlist_t remote(VRAM_SEG);
    const std::string peer = "peer";
    const nixl::trace::TraceContext traceContext;
    nixl_opt_b_args_t args{.notifMsg = "notification",
                           .hasNotif = true,
                           .customParam = "custom",
                           .traceContext = &traceContext};
    nixlBackendReqH prepared;
    nixlBackendReqH posted;
    nixlBackendReqH *handle = nullptr;
    const auto emptyHandle = AllOf(Ref(handle), Eq(nullptr));
    const auto preparedHandle = AllOf(Ref(handle), Eq(&prepared));
    EXPECT_CALL(*engine,
                prepXfer(Ref(operation), Ref(local), Ref(remote), Ref(peer), emptyHandle, &args))
        .WillOnce(DoAll(SetArgReferee<4>(&prepared), Return(status)));
    EXPECT_CALL(*engine,
                postXfer(Ref(operation), Ref(local), Ref(remote), Ref(peer), preparedHandle, &args))
        .WillOnce(DoAll(SetArgReferee<4>(&posted), Return(status)));
    EXPECT_CALL(*engine, checkXfer(&posted)).WillOnce(Return(status));
    EXPECT_CALL(*engine, releaseReqH(&posted)).WillOnce(Return(status));
    ForwardingEngine wrapper(&params, std::move(engine));
    const nixlBackendEngine &base = wrapper;
    EXPECT_EQ(base.prepXfer(operation, local, remote, peer, handle, &args), status);
    EXPECT_EQ(handle, &prepared);
    EXPECT_EQ(base.postXfer(operation, local, remote, peer, handle, &args), status);
    EXPECT_EQ(handle, &posted);
    EXPECT_EQ(base.checkXfer(handle), status);
    EXPECT_EQ(base.releaseReqH(handle), status);
}

TEST_F(ForwardingEngineTest, PreservesOptionalMethodOutputs) {
    const auto status = NIXL_SUCCESS;
    auto engine = makeEngine();
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
        *engine,
        prepMemView(testing::Matcher<const nixl_meta_dlist_t &>(Ref(local)), Ref(view), &args))
        .WillOnce(DoAll(SetArgReferee<1>(&localView), Return(status)));
    EXPECT_CALL(*engine,
                prepMemView(testing::Matcher<const nixl_remote_meta_dlist_t &>(Ref(remote)),
                            Ref(view),
                            &args))
        .WillOnce(DoAll(SetArgReferee<1>(&remoteView), Return(status)));
    EXPECT_CALL(*engine, releaseMemView(&localView));
    EXPECT_CALL(*engine, releaseMemView(&remoteView));
    EXPECT_CALL(*engine, getNotifs(Ref(notifications)))
        .WillOnce(DoAll(SetArgReferee<0>(notif_list_t{{peer, message}}), Return(status)));
    EXPECT_CALL(*engine, genNotif(Ref(peer), Ref(message))).WillOnce(Return(status));
    EXPECT_CALL(*engine, queryMem(Ref(registration), Ref(responses)))
        .WillOnce(DoAll(
            SetArgReferee<1>(std::vector<nixl_query_resp_t>{nixl_b_params_t{{"size", "4096"}}}),
            Return(status)));
    EXPECT_CALL(*engine,
                estimateXferCost(Ref(operation),
                                 Ref(local),
                                 Ref(remoteTransfer),
                                 Ref(peer),
                                 Ref(handle),
                                 Ref(duration),
                                 Ref(margin),
                                 Ref(method),
                                 &costArgs))
        .WillOnce(DoAll(SetArgReferee<5>(std::chrono::microseconds{123}),
                        SetArgReferee<6>(std::chrono::microseconds{7}),
                        SetArgReferee<7>(nixl_cost_t::ANALYTICAL_BACKEND),
                        Return(status)));
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

} // namespace
