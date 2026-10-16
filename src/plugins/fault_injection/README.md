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

`forwarding_engine.h` provides `nixl::fault::ForwardingEngine`, a header-only
helper for the planned fault-injection proxy. It forwards every current
`nixlBackendEngine` virtual method, including optional methods and both
memory-view overloads.

## Ownership and use

The proxy plugin will create the genuine engine with the original initialization
parameters, move it into `ForwardingEngine`, and return the wrapper to NIXL.
vLLM will continue requesting UCX normally.

```cpp
const nixl::fault::EngineDeleter destroy(plugin->destroy_engine);
nixl::fault::EnginePtr engine(plugin->create_engine(&params), destroy);
auto forwarding = std::make_unique<nixl::fault::ForwardingEngine>(
    &params, std::move(engine));
```

`EnginePtr` owns the engine. Supply the genuine plugin's `destroy_engine` callback
through `EngineDeleter`; its default uses `delete` for directly allocated engines,
including test mocks. Keep the plugin loaded until engine destruction completes.
Cleanup runs once, including when wrapper construction fails.

Null initialization parameters, a null engine or destroy callback, and mismatched
backend types or parameter maps throw `std::invalid_argument`. A null parameter
map is valid. Pass the same initialization parameters to both engines.

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
the interface grows; new pure virtual methods and changed overridden signatures
also fail compilation.
