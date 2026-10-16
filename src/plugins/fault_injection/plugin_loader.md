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

`PluginLoader` loads a genuine NIXL backend from an explicit absolute shared-library
path. The future proxy plugin will use it to create real backend engines while
preserving the backend's reported identity and options. This component does not
perform NIXL's name-based discovery or register itself as a backend.

## Use and ownership

```cpp
nixl::fault::PluginLoader plugin("/opt/nixl/real/libplugin_UCX.so");
auto engine = plugin.createEngine(&params);
```

The loader passes the original initialization-parameter pointer to the plugin's
`create_engine` callback. It preserves the resulting engine, including its
initialization-error flag. Callers must check that flag as they normally would.
A null parameter map is valid; a null initialization-parameter pointer is rejected.

The returned `PluginLoader::EnginePtr` owns the engine. Its deleter calls the
genuine plugin's `destroy_engine` callback and retains shared ownership of the
loaded library. Engines can outlive the loader. Loader copies share one loaded
state, initialization, and finalizer. Never release an engine from its owning
pointer without also preserving the deleter and library lifetime.

Resetting an engine pointer destroys the engine; its deleter retains the library
until the owning pointer itself is destroyed. After the last shared owner goes
away, the loader calls `nixl_plugin_fini`, if present, then `dlclose`.

Loading uses `RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE`. Optional `deepbind = true`
adds `RTLD_DEEPBIND` (default off); the proxy applies core's `NIXL_UCX_DEEPBIND`
policy. Deep binding prefers plugin symbols over process symbols, bypassing
`LD_PRELOAD` and sanitizer interposition. Enable only when needed; unsupported
platforms warn and continue without it.

`RTLD_NODELETE` keeps code resident to avoid Abseil/TLS unloading problems on older
glibc. Finalization and handle release still occur in ownership order; physical
unmapping is not promised. Plugin destroy and finalizer callbacks must not throw.

## Validation and failures

Loading rejects relative or empty paths, a path that resolves to the shared object
containing the loader (the proxy itself, including via symlink), missing libraries or initialization
symbols, failed initialization, API-version mismatches, missing required
callbacks, and null or empty plugin names and versions. The required callbacks
are `create_engine`, `destroy_engine`, `get_plugin_name`, and `get_plugin_version`.
The version check rejects a reported API mismatch; it cannot prove C++ ABI or
toolchain compatibility. Build the proxy and backend against matching interfaces.

The optional `get_backend_options` and `get_backend_mems` callbacks default to
empty results when absent, matching core behavior. The finalizer is optional too.

Loading failures and null factory results throw `std::runtime_error` with the
library path. Standard exceptions from initialization and factory callbacks are
reported with their original message and library path. Invalid input paths (including a self-referencing one) and
null initialization parameters throw `std::invalid_argument`.

A successful, non-null initialization result owns a matching finalizer call,
including when subsequent validation fails. An initializer that returns null or
throws must clean up its own partial state, as the loader has no valid plugin
instance to finalize. Failed engine creation must likewise clean up partial
engine allocations inside the plugin.

## Validation tests

From the repository root, with NIXL build dependencies and Google Test/Mock:

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=POSIX -Dbuild_examples=false -Dwith_trace=false
meson test -C build plugin_loader --print-errorlogs
```

For ASan/UBSan validation with this workspace's unsanitized system Abseil, add
`-Dsanitizer=address,undefined -Dcpp_args=-DNDEBUG_SANITIZER` at setup. The latter
keeps Abseil's hash-table layout compatible with the installed library by disabling
its extra sanitizer generation metadata; ASan and UBSan remain enabled.

The test builds the loader source and small fixture shared libraries. It checks
plugin metadata, required and optional callbacks, parameter-pointer identity,
failed initialization and factories, initialization-error preservation, callback
cleanup, copied loaders, moved engine owners, and multiple engines outliving
their loader. Lifecycle assertions observe finalization explicitly rather than
assuming that `dlclose` unmaps a library.

No GPUs, real UCX plugin, peer processes, or inference framework are needed.
This PR does not wire the loader into `ForwardingEngine`, install a proxy under
UCX's filename, select UCX-specific deep binding, parse TOML, or inject faults.
The loader's owning pointer carries library lifetime; future forwarder integration
must retain that ownership rather than copy only a raw engine and destroy callback.
