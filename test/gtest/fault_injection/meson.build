# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

bad_api_plugin_fixture = shared_library(
    'backend_plugin_bad_api_fixture',
    'backend_plugin_fixture.cpp',
    cpp_args: ['-DNIXL_TEST_PLUGIN_BAD_API'],
    dependencies: [nixl_common_dep],
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    install: false,
)

missing_callback_plugin_fixture = shared_library(
    'backend_plugin_missing_callback_fixture',
    'backend_plugin_fixture.cpp',
    cpp_args: ['-DNIXL_TEST_PLUGIN_MISSING_CALLBACK'],
    dependencies: [nixl_common_dep],
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    install: false,
)

fault_injection_test_sources = files(
    '../../fault_injection/backend_engine_forwarder.cpp',
    '../../fault_injection/backend_plugin_loader.cpp',
    'backend_engine_forwarder_test.cpp',
    'backend_plugin_loader_test.cpp',
)

fault_injection_test_inc = include_directories('../../fault_injection')
fault_injection_test_deps = [dl_dep]
