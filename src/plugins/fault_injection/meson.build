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

fault_proxy = shared_library('nixl_fault_proxy',
    ['proxy_plugin.cpp', 'plugin_loader.cpp'],
    include_directories: [nixl_inc_dirs, utils_inc_dirs],
    dependencies: [nixl_infra, nixl_common_dep, dl_dep],
    gnu_symbol_visibility: 'hidden',
    build_by_default: get_option('build_fault_injection'),
    install: get_option('build_fault_injection'),
    install_dir: plugin_install_dir / 'fault_injection',
    install_rpath: '$ORIGIN/../..')
