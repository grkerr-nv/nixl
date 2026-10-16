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

#include "plugin_loader.h"

#include <exception>
#include <string>

// Shared object embedding the loader, so a test can point the loader at its own container.
extern "C" __attribute__((visibility("default"))) const char *
loader_self_load_probe(const char *path) {
    static std::string message;
    try {
        nixl::fault::PluginLoader loader(path);
        message = "loaded";
    }
    catch (const std::exception &error) {
        message = error.what();
    }
    return message.c_str();
}
