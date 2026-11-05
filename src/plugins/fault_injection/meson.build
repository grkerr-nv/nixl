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

plugin_loader_lib = static_library('nixl_plugin_loader',
    'plugin_loader.cpp',
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_infra, nixl_common_dep, dl_dep],
    build_by_default: get_option('build_fault_injection'),
    pic: true)

nixl_plugin_loader_dep = declare_dependency(
    link_with: plugin_loader_lib,
    include_directories: [nixl_inc_dirs, utils_inc_dirs, include_directories('.')],
    dependencies: [nixl_infra, nixl_common_dep, dl_dep])

fault_config_lib = static_library('nixl_fault_config',
    'fault_config.cpp',
    'table_scope.cpp',
    include_directories: nixl_inc_dirs,
    dependencies: tomlplusplus_dep,
    build_by_default: get_option('build_fault_injection'),
    pic: true)

# table_scope.h is toml++-facing, so consumers that test it need the header path.
# fault_config.h itself stays free of toml++.
nixl_fault_config_dep = declare_dependency(
    link_with: fault_config_lib,
    include_directories: [nixl_inc_dirs, include_directories('.')],
    dependencies: tomlplusplus_dep)
