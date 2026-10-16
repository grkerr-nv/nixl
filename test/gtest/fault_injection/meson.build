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
loader_fixture_mocks = static_library('loader_fixture_mocks',
    '../mocks/gmock_engine.cpp',
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_common_dep, gmock_dep],
    pic: true)

loader_fixture = shared_library('loader_fixture',
    'plugin_loader_fixture.cpp',
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_infra, nixl_common_dep, gmock_dep],
    link_with: loader_fixture_mocks)

loader_fixture_no_fini = shared_library('loader_fixture_no_fini',
    'plugin_loader_fixture.cpp',
    cpp_args: ['-DLOADER_FIXTURE_NO_FINI'],
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_infra, nixl_common_dep, gmock_dep],
    link_with: loader_fixture_mocks)

loader_fixture_missing_init = shared_library('loader_fixture_missing_init',
    'plugin_loader_missing_init.cpp')

# Embeds the loader so it can be pointed at the shared object that contains it.
# Hidden visibility keeps the embedded loader's symbols local to this library.
loader_self_load = shared_library('loader_self_load',
    ['plugin_loader_self_load.cpp', '../../../src/plugins/fault_injection/plugin_loader.cpp'],
    include_directories: [nixl_inc_dirs, utils_inc_dirs,
                         include_directories('../../../src/plugins/fault_injection')],
    gnu_symbol_visibility: 'hidden',
    dependencies: [nixl_infra, nixl_common_dep, dl_dep])

plugin_loader_test = executable('plugin_loader_test',
    ['plugin_loader_test.cpp', '../../../src/plugins/fault_injection/plugin_loader.cpp'],
    cpp_args: [
        '-DLOADER_FIXTURE_PATH="' + loader_fixture.full_path() + '"',
        '-DLOADER_NO_FINI_PATH="' + loader_fixture_no_fini.full_path() + '"',
        '-DLOADER_MISSING_INIT_PATH="' + loader_fixture_missing_init.full_path() + '"',
        '-DLOADER_SELF_LOAD_PATH="' + loader_self_load.full_path() + '"',
    ],
    include_directories: [nixl_inc_dirs, utils_inc_dirs,
                         include_directories('../../../src/plugins/fault_injection')],
    dependencies: [nixl_infra, nixl_common_dep, dl_dep,
                   dependency('gtest', version: '>=1.11.0', main: true)])

test('plugin_loader', plugin_loader_test,
     depends: [loader_fixture, loader_fixture_no_fini, loader_fixture_missing_init,
                loader_self_load],
     env: sanitizer_env, suite: ['resiliency', 'sanitizer'])
