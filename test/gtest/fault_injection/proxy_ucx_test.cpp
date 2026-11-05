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

// Exercises normal plugin discovery and inert proxy forwarding to real UCX with a
// 4 KiB DRAM transfer between two agents in one process. Checks data and cleanup;
// does not test fault injection, multi-node behavior, or proxy overhead.

#include "nixl.h"
#include "../common.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Dedicated test process: configuration and plugin discovery are process-wide.
class proxyUcxTest : public testing::Test {
protected:
    void
    SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "nixl-proxy-ucx-XXXXXX").string();
        const char *created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        directory_ = created;
        std::filesystem::create_symlink(PROXY_PLUGIN_PATH, directory_ / "libplugin_UCX.so");
        const auto config = directory_ / "config.toml";
        std::ofstream file(config);
        ASSERT_TRUE(file);
        file << "[fault_injection]\nschema_version = 1\nexperiment_id = 'ucx-smoke'\n"
             << "real_plugin_path = '" << REAL_UCX_PLUGIN_PATH << "'\n";
        env_.addVar("NIXL_FAULT_CONFIG_FILE", config.string());
        env_.addVar("NIXL_PLUGIN_DIR", directory_.string());
    }

    void
    TearDown() override {
        // env_ restores both variables before the directory they point into is removed.
        if (!directory_.empty()) {
            std::filesystem::remove_all(directory_);
        }
    }

private:
    std::filesystem::path directory_;
    gtest::ScopedEnv env_;
};

TEST_F(proxyUcxTest, TransfersDramBetweenAgents) {
    std::vector<char> source(4096, 'x'), destination(source.size(), 0);
    nixlAgentConfig config;
    config.useProgThread = true;
    nixlAgent sender("proxy-sender", config), receiver("proxy-receiver", config);
    nixlBackendH *sender_backend = nullptr, *receiver_backend = nullptr;
    ASSERT_EQ(sender.createBackend("UCX", {}, sender_backend), NIXL_SUCCESS);
    ASSERT_EQ(receiver.createBackend("UCX", {}, receiver_backend), NIXL_SUCCESS);

    nixl_reg_dlist_t source_regs(DRAM_SEG), destination_regs(DRAM_SEG);
    const auto source_address = reinterpret_cast<uintptr_t>(source.data());
    const auto destination_address = reinterpret_cast<uintptr_t>(destination.data());
    source_regs.addDesc(nixlBlobDesc(source_address, source.size(), 0));
    destination_regs.addDesc(nixlBlobDesc(destination_address, destination.size(), 0));
    ASSERT_EQ(sender.registerMem(source_regs), NIXL_SUCCESS);
    ASSERT_EQ(receiver.registerMem(destination_regs), NIXL_SUCCESS);
    std::string metadata, peer;
    ASSERT_EQ(receiver.getLocalMD(metadata), NIXL_SUCCESS);
    ASSERT_EQ(sender.loadRemoteMD(metadata, peer), NIXL_SUCCESS);
    ASSERT_EQ(peer, "proxy-receiver");

    nixl_xfer_dlist_t local(DRAM_SEG), remote(DRAM_SEG);
    local.addDesc(nixlBasicDesc(source_address, source.size(), 0));
    remote.addDesc(nixlBasicDesc(destination_address, destination.size(), 0));
    nixlXferReqH *request = nullptr;
    ASSERT_EQ(sender.createXferReq(NIXL_WRITE, local, remote, peer, request), NIXL_SUCCESS);
    auto status = sender.postXferReq(request);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (status == NIXL_IN_PROG && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        status = sender.getXferStatus(request);
    }
    // Stop here on a timeout: releasing and deregistering under an in-flight transfer
    // would hand UCX freed memory.
    ASSERT_EQ(status, NIXL_SUCCESS);
    EXPECT_EQ(destination, source);
    EXPECT_EQ(sender.releaseXferReq(request), NIXL_SUCCESS);
    EXPECT_EQ(sender.invalidateRemoteMD(peer), NIXL_SUCCESS);
    EXPECT_EQ(sender.deregisterMem(source_regs), NIXL_SUCCESS);
    EXPECT_EQ(receiver.deregisterMem(destination_regs), NIXL_SUCCESS);
}

} // namespace
