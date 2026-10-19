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

# Resiliency backend proxy

The inert proxy connects `pluginLoader` to `forwardingEngine` through the normal
NIXL plugin entrypoints. It preserves the genuine plugin's name, version, backend
options, memory types, engine initialization parameters, and forwarding behavior.
It requires the existing toml++ and dynamic-loader dependencies; UCX is needed
only when UCX is the genuine backend.

## Build and deployment

Build and install explicitly with `-Dbuild_fault_injection=true`. The default
leaves the proxy uninstalled; focused tests may still build it.

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=UCX -Dbuild_fault_injection=true -Dbuild_examples=false -Dwith_trace=false
meson compile -C build nixl_fault_proxy
```

The installed library lives in `<libdir>/plugins/fault_injection`, separately
from genuine plugins. Select it using a backend-name symlink in a dedicated
directory. For a build-tree UCX experiment:

```bash
mkdir -p /tmp/nixl-proxy-plugins
ln -s "$PWD/build/src/plugins/fault_injection/libnixl_fault_proxy.so" /tmp/nixl-proxy-plugins/libplugin_UCX.so
export NIXL_PLUGIN_DIR=/tmp/nixl-proxy-plugins
export NIXL_FAULT_CONFIG_FILE=/absolute/path/to/inert.toml
```

Use `debugoptimized` for this discovery path: debug builds may prioritize their
generated `pluginlist` over `NIXL_PLUGIN_DIR`. Statically built-in backends also
bypass dynamic discovery. Start a fresh process after changing configuration.
Keep the framework's backend selection as `UCX`.

```toml
[fault_injection]
schema_version = 1
experiment_id = "inert-ucx"
real_plugin_path = "/absolute/path/to/genuine/libplugin_UCX.so"
rules = []
```

`real_plugin_path` must point to the genuine library, never the proxy or its
symlink. The configuration file is required even with no injection rules because
the proxy cannot infer the genuine library's location. Missing or invalid
configuration fails plugin initialization with a NIXL error log. Unknown keys,
unsupported schema versions, empty experiment IDs, and nonempty or malformed
rules are rejected. Injection is not implemented in this chunk.

The proxy reads configuration once per initialized lifetime. Matching calls to
`nixl_plugin_init` and `nixl_plugin_fini` share that state. Genuine engines retain
their plugin until their destruction, including after proxy finalization.
Matching initialization/finalization calls own the callback state explicitly,
avoiding C++ static-destructor ordering during late core shutdown.
Factory exceptions become logged errors and null results. When discovered as
`libplugin_UCX.so`, the proxy applies core's `NIXL_UCX_DEEPBIND` policy to the
genuine library. See [loader limitations](plugin_loader.md).

Retarget by changing the discovery symlink to `libplugin_<backend>.so` and
`real_plugin_path`; the proxy library needs no rebuild. The symlink name must
match the genuine backend name.

## Forwarding helper

`forwarding_engine.h` provides `nixl::fault::forwardingEngine`, a header-only
helper for the planned fault-injection proxy. It forwards every current
`nixlBackendEngine` virtual method, including optional methods and both
memory-view overloads.

## Ownership and use

The proxy plugin will create the genuine engine with the original initialization
parameters, move it into `forwardingEngine`, and return the wrapper to NIXL.
vLLM will continue requesting UCX normally.

```cpp
const nixl::fault::engineDeleter destroy(plugin->destroy_engine, plugin_owner);
nixl::fault::engine_ptr_t engine(plugin->create_engine(&params), destroy);
auto forwarding = std::make_unique<nixl::fault::forwardingEngine>(
    &params, std::move(engine));
```

The loader and forwarder share `engine_ptr_t`, defined in `engine_ptr.h` as
`std::unique_ptr<nixlBackendEngine, engineDeleter>`. Moving it into the constructor
transfers exclusive engine ownership with its original deleter. Pass
`loader.createEngine(&params)` directly, without releasing the raw pointer.

`engine_ptr_t` owns the engine. Supply the genuine plugin's `destroy_engine` callback
through `engineDeleter`, along with `plugin_owner`, a shared owner that finalizes
and unloads the plugin. The deleter retains that owner until engine cleanup has
finished. Its default uses `delete` for directly allocated engines, including
mocks. Cleanup runs once, including when wrapper construction fails.

Null initialization parameters, a null engine, missing plugin ownership or destroy
callback, and conflicting backend types or parameter values throw
`std::invalid_argument`. A null parameter map is valid. Engine-discovered keys are
adopted; caller-only keys added during engine initialization are preserved.

Calls preserve arguments, return values, and output references. Metadata, request
handles, and memory views stay genuine-backend objects. Release them through the
corresponding methods before destroying the engine.

## Limitations

Nonvirtual getters expose a construction-time snapshot of the genuine engine's
initialization error, type, and parameters, including discovered parameters.

Telemetry-enabled initialization is accepted. The nonvirtual telemetry getter
reads the wrapper's own empty queue. No backend currently calls
`addTelemetryEvent`, and core does not call `getTelemetryEvents`. If that changes,
drain the genuine engine's public `getTelemetryEvents()` into the wrapper queue.

Fault rule evaluation, injection, and overhead measurement remain separate work.
Functional tests do not prove the design's
statistical non-perturbation requirement.

## Validation

With NIXL dependencies and Google Test/Mock installed:

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=POSIX -Dbuild_examples=false -Dwith_trace=false
meson compile -C build forwarding_engine_test
meson test -C build --no-rebuild forwarding_engine --print-errorlogs
```

For focused component and proxy coverage:

```bash
meson test -C build plugin_loader forwarding_engine proxy_plugin proxy_discovery --print-errorlogs
```

With a dynamic UCX backend and a non-debug build,
`meson test -C build proxy_ucx --print-errorlogs` also exercises a two-agent,
same-host DRAM transfer through normal NIXL discovery. UCX must be able to open
its transport sockets; restricted sandboxes may prevent this test from running.
This smoke test does not establish multi-node behavior or inert-proxy overhead.

Forwarding-helper tests need no GPUs, network peers, or inference framework. They cover every
current virtual method, argument identity, representative statuses, outputs,
opaque handles, initialization, and cleanup. Extend the wrapper and tests when
the interface grows. A build check compares virtual method names and overload
counts, including optional methods. C++ `override` checks enforce signatures.
