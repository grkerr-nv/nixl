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

`PluginLoader` loads a genuine backend by absolute path for the future fault proxy.
It exposes plugin metadata and creates engines; it does not register a proxy or
perform name-based discovery.

## Use and ownership

```cpp
nixl::fault::PluginLoader plugin("/opt/nixl/real/libplugin_UCX.so");
auto engine = plugin.createEngine(&params);
```

The loader is noncopyable. It passes `params` unchanged and preserves the engine's
initialization-error flag; callers must check `getInitErr()`. A null parameter map
is valid; a null `params` pointer is rejected.

`EnginePtr` calls the plugin's `destroy_engine` callback and retains library
ownership, so engines can outlive the loader. Resetting the pointer destroys the
engine; its deleter retains ownership until the pointer itself is destroyed.
The last owner calls optional `nixl_plugin_fini`, then `dlclose`. Destroy and
finalizer callbacks must not throw. Never release an engine without retaining
its deleter and library ownership.

## Loading and failures

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

## Tests

With NIXL build dependencies and Google Test/Mock:

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=POSIX -Dbuild_examples=false -Dwith_trace=false
meson test -C build plugin_loader --print-errorlogs
```

Fixture plugins test metadata, validation, parameter identity, initialization
errors, factory failures, copy prevention, ownership, and cleanup order.
No GPUs, real UCX plugin, peers, or inference framework needed.

For this workspace's unsanitized system Abseil, add
`-Dsanitizer=address,undefined -Dcpp_args=-DNDEBUG_SANITIZER` at setup.
`NDEBUG_SANITIZER` disables extra Abseil generation metadata to preserve its
hash-table ABI; ASan and UBSan remain enabled.

Forwarder integration, proxy installation, UCX binding-policy selection, TOML,
and fault injection remain separate work.
