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

# Resiliency forwarding engine

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

Plugin loading, plugin-level identity, fault configuration, injection, and overhead
measurement remain separate work. Functional tests do not prove the design's
statistical non-perturbation requirement.

## Validation

With NIXL dependencies and Google Test/Mock installed:

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=POSIX -Dbuild_examples=false -Dwith_trace=false
meson compile -C build forwarding_engine_test
meson test -C build --no-rebuild forwarding_engine --print-errorlogs
```

Tests need no GPUs, network peers, or inference framework. They cover every
current virtual method, argument identity, representative statuses, outputs,
opaque handles, initialization, and cleanup. Extend the wrapper and tests when
the interface grows. A build check compares virtual method names and overload
counts, including optional methods. C++ `override` checks enforce signatures.
