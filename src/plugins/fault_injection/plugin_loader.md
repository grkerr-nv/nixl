<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Genuine backend plugin loader

`pluginLoader` loads a genuine backend by absolute path for the future fault proxy.
It exposes plugin metadata and creates engines; it does not register a proxy or
perform name-based discovery.

## Use and ownership

```cpp
nixl::fault::pluginLoader plugin("/opt/nixl/real/libplugin_UCX.so");
nixl::fault::forwardingEngine proxy(&params, plugin.createEngine(&params));
```

Keep the genuine library outside all core discovery directories (including
`NIXL_PLUGIN_DIR`) and out of the plugin list. Expose only the proxy to core
to avoid duplicate initialization/finalization. The loader does not enforce
this separation.

The loader is neither copyable nor movable. It passes `params` unchanged and
preserves the engine's initialization-error flag; callers must check `getInitErr()`.
A null parameter map is valid; a null `params` pointer is rejected.

`getName()` and `getVersion()` return borrowed `const char *` metadata directly
from the genuine plugin. Keep the loader alive while using those pointers;
copy them into `std::string` if ownership is needed.

The shared `nixl::fault::engine_ptr_t` passes directly into `forwardingEngine`.
It calls the plugin's `destroy_engine` callback and retains library
ownership, so engines can outlive the loader. Resetting the pointer destroys the
engine; its deleter retains ownership until the pointer itself is destroyed.
The last owner calls optional `nixl_plugin_fini`, then `dlclose`. Cleanup exceptions
are logged and suppressed without retry. Never release an engine without retaining
its deleter and library ownership.

## Loading and failures

Loading stays local so the proxy can run against an unmodified NIXL build.
Core's explicit-path loader is private; its public name-based lookup would
resolve the proxy again. A new core factory would make the proxy depend on a
modified NIXL build. Keep this sequence aligned with core's loading flags,
deep-binding fallback, API check, and finalizer/`dlclose` order.

Loading uses `RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE`. `RTLD_NODELETE` avoids
Abseil/TLS unloading problems on older glibc; cleanup does not promise unmapping.

Optional `deepbind = true` adds `RTLD_DEEPBIND` (default off). The proxy applies
core's `NIXL_UCX_DEEPBIND` policy. Deep binding prefers plugin symbols, bypassing
`LD_PRELOAD` and sanitizer interposition; enable only when needed. Unsupported
platforms warn and continue without it.

- `std::invalid_argument`: empty or relative paths, self-loading (including
  symlinks), or null initialization parameters.
- `std::runtime_error`: missing libraries or initialization symbols, failed
  initialization, API mismatches, missing required callbacks, empty names or
  versions, or failed engine creation. Diagnostics include the library path;
  standard exceptions from initializers and factories retain their message.

Required callbacks: `create_engine`, `destroy_engine`, `get_plugin_name`, and
`get_plugin_version`. Names and versions must be non-null. Optional
`get_backend_options` and `get_backend_mems` return empty results when absent.
The API check does not prove C++ ABI compatibility; use matching interfaces and
toolchains.

Successful initialization gets a matching finalizer call, even if validation
fails. Initializers that return null or throw must clean up partial state;
failed factories must clean up partial engine allocations.

## Build and tests

The `nixl_plugin_loader` static library builds independently of tests, including
when `build_tests=false`. In-tree consumers use `nixl_plugin_loader_dep` to inherit
its headers and link dependencies. Loader tests link the same library.

With NIXL build dependencies and Google Test/Mock:

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=POSIX -Dbuild_examples=false -Dwith_trace=false
meson test -C build plugin_loader --print-errorlogs
```

Fixture plugins test metadata, validation, parameter identity, initialization
errors, factory and cleanup failures, copy prevention, ownership, and cleanup order.
Self-load rejection covers the test executable, a symlink to it, and a loaded
fixture whose original alias has been deleted. A deep-binding request test reuses
the existing fixture; it skips sanitizer builds because
`RTLD_DEEPBIND` bypasses sanitizer interposition.
No GPUs, real UCX plugin, peers, or inference framework needed.

For this workspace's unsanitized system Abseil, add
`-Dsanitizer=address,undefined -Dcpp_args=-DNDEBUG_SANITIZER` at setup.
`NDEBUG_SANITIZER` disables extra Abseil generation metadata to preserve its
hash-table ABI; ASan and UBSan remain enabled.

Proxy installation, UCX binding-policy selection, TOML, and fault injection
remain separate work.
