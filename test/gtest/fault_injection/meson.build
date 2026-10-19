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

# Compile shared mock code once for both valid fixture variants.
# Keep one Google Mock runtime for fixtures and host-side expectations.
loader_fixture_mocks = shared_library('loader_fixture_mocks',
    '../mocks/gmock_engine.cpp',
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_infra, nixl_common_dep, gmock_dep])

loader_fixture = shared_library('loader_fixture',
    'plugin_loader_fixture.cpp',
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_plugin_loader_dep, gmock_dep],
    link_with: loader_fixture_mocks)

loader_fixture_no_fini = shared_library('loader_fixture_no_fini',
    'plugin_loader_fixture.cpp',
    cpp_args: ['-DLOADER_FIXTURE_NO_FINI'],
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_plugin_loader_dep, gmock_dep],
    link_with: loader_fixture_mocks)

plugin_loader_test = executable('plugin_loader_test',
    'plugin_loader_test.cpp',
    'fault_config_test.cpp',
    'table_scope_test.cpp',
    link_args: ['-Wl,--wrap=dlerror'],
    cpp_args: [
        '-DLOADER_FIXTURE_PATH="' + loader_fixture.full_path() + '"',
        '-DLOADER_NO_FINI_PATH="' + loader_fixture_no_fini.full_path() + '"',
        '-DLOADER_TEST_SANITIZED=' + (get_option('sanitizer') == 'none' ? '0' : '1'),
    ],
    dependencies: [nixl_plugin_loader_dep, nixl_fault_config_dep,
                   dependency('gtest', version: '>=1.11.0', main: true)])

test('plugin_loader', plugin_loader_test,
     depends: [loader_fixture, loader_fixture_no_fini],
     env: sanitizer_env, suite: ['resiliency', 'sanitizer'])

proxy_plugin_test = executable('proxy_plugin_test',
    'proxy_plugin_test.cpp',
    cpp_args: [
        '-DLOADER_FIXTURE_PATH="' + loader_fixture.full_path() + '"',
        '-DPROXY_PLUGIN_PATH="' + fault_proxy.full_path() + '"',
    ],
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    link_with: loader_fixture_mocks,
    dependencies: [nixl_dep, nixl_infra, nixl_common_dep, dl_dep, gmock_dep,
                   dependency('gtest', version: '>=1.11.0', main: true)])

test('proxy_plugin', proxy_plugin_test,
     args: ['--gtest_filter=-*.CoreDiscovery'],
     depends: [fault_proxy, loader_fixture],
     env: sanitizer_env, suite: ['resiliency', 'sanitizer'])
test('proxy_discovery', proxy_plugin_test,
     args: ['--gtest_filter=*.CoreDiscovery'],
     depends: [fault_proxy, loader_fixture],
     env: sanitizer_env, suite: ['resiliency', 'sanitizer'])

if is_variable('ucx_backend_lib') and 'UCX' not in static_plugins and get_option('buildtype') != 'debug'
    proxy_ucx_test = executable('proxy_ucx_test',
        'proxy_ucx_test.cpp',
        cpp_args: [
            '-DPROXY_PLUGIN_PATH="' + fault_proxy.full_path() + '"',
            '-DREAL_UCX_PLUGIN_PATH="' + ucx_backend_lib.full_path() + '"',
        ],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        dependencies: [nixl_dep, nixl_infra, nixl_common_dep,
                       dependency('gtest', version: '>=1.11.0', main: true)])
    test('proxy_ucx', proxy_ucx_test,
         depends: [fault_proxy, ucx_backend_lib],
         env: sanitizer_env, suite: ['resiliency', 'sanitizer'], timeout: 30)
endif
