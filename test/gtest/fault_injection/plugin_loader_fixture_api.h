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

// Shared failure modes and lifecycle events for the loader tests and fixture plugin.

#ifndef NIXL_TEST_GTEST_FAULT_INJECTION_PLUGIN_LOADER_FIXTURE_API_H
#define NIXL_TEST_GTEST_FAULT_INJECTION_PLUGIN_LOADER_FIXTURE_API_H

#include "backend/backend_aux.h"

namespace loader_fixture {

enum class fixture_mode_t {
    VALID,
    NULL_INIT,
    THROW_INIT,
    OLD_API,
    NO_CREATE,
    NO_DESTROY,
    NO_NAME_CALLBACK,
    NO_VERSION_CALLBACK,
    NULL_NAME,
    EMPTY_NAME,
    NULL_VERSION,
    EMPTY_VERSION,
    NO_OPTIONS,
    NO_MEMS,
    NO_OPTIONAL,
    NULL_ENGINE,
    THROW_CREATE,
    ENGINE_INIT_ERROR,
};

enum class fixture_event_t { INIT, CREATE, DESTROY_CALLBACK, ENGINE_DESTROYED, FINALIZE };

struct observer {
    void *context = nullptr; // Pass test state to the callback without exposing its type.
    void (*record)(void *, fixture_event_t, const nixlBackendInitParams *) = nullptr;
};

using configure_function_t = void(fixture_mode_t, observer);
using configure_t = configure_function_t *;

} // namespace loader_fixture

#endif
