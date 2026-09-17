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

# One plugin per way of being broken, plus an unadorned one that works.
backend_plugin_fixture_variants = {
    'good': [],
    'bad_api': ['-DNIXL_TEST_PLUGIN_BAD_API'],
    'missing_callback': ['-DNIXL_TEST_PLUGIN_MISSING_CALLBACK'],
    'no_optional': ['-DNIXL_TEST_PLUGIN_NO_OPTIONAL'],
    'null_engine': ['-DNIXL_TEST_PLUGIN_NULL_ENGINE'],
    'null_init': ['-DNIXL_TEST_PLUGIN_NULL_INIT'],
    'no_init': ['-DNIXL_TEST_PLUGIN_NO_INIT'],
}

backend_plugin_fixtures = []
foreach variant, variant_args : backend_plugin_fixture_variants
    backend_plugin_fixtures += shared_library(
        'backend_plugin_' + variant + '_fixture',
        'backend_plugin_fixture.cpp',
        cpp_args: variant_args,
        dependencies: [nixl_common_dep],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: false,
    )
endforeach

# The fixtures are dlopened rather than linked, so no target would otherwise
# build them. Publishing their directory through a generated header gives the
# test the path and makes compiling the test wait on the fixtures, which
# executable() cannot express directly: it has no depends: keyword.
backend_plugin_fixture_dir = configuration_data()
backend_plugin_fixture_dir.set_quoted(
    'NIXL_TEST_PLUGIN_FIXTURE_DIR',
    meson.current_build_dir(),
)

# configure_file writes the contents but runs at configure time, so it cannot
# depend on the fixtures; copying it through a custom_target adds that edge.
backend_plugin_fixture_dir_in = configure_file(
    output: 'backend_plugin_fixture_dir.h.in',
    configuration: backend_plugin_fixture_dir,
)

backend_plugin_fixture_dir_h = custom_target(
    'backend_plugin_fixture_dir.h',
    input: backend_plugin_fixture_dir_in,
    output: 'backend_plugin_fixture_dir.h',
    depends: backend_plugin_fixtures,
    command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
)

fault_injection_test_sources = files(
    '../../fault_injection/backend_engine_forwarder.cpp',
    '../../fault_injection/backend_plugin_loader.cpp',
    'backend_engine_forwarder_test.cpp',
    'backend_plugin_loader_test.cpp',
)
fault_injection_test_sources += backend_plugin_fixture_dir_h

fault_injection_test_inc = include_directories('../../fault_injection')
fault_injection_generated_inc = include_directories('.')
fault_injection_test_deps = [dl_dep]
