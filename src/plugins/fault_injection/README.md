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

`forwarding_engine.h` provides `nixl::fault::ForwardingEngine`, the inert engine
layer for the planned fault-injection proxy. It is a header-only internal helper,
not a loadable plugin. It overrides every current `nixlBackendEngine` virtual
method, including optional methods and both memory-view overloads.

## Ownership and use

Create the genuine engine with the original initialization parameters, then move
it into `ForwardingEngine` using `nixl::fault::EnginePtr`. For plugin-created
engines, supply the plugin's `destroy_engine` callback through
`nixl::fault::EngineDeleter`. The default deleter uses `delete` and is suitable
for directly allocated engines, including test mocks.

```cpp
const nixl::fault::EngineDeleter destroy(plugin->destroy_engine);
nixl::fault::EnginePtr engine(plugin->create_engine(&params), destroy);
auto forwarding = std::make_unique<nixl::fault::ForwardingEngine>(
    &params, std::move(engine));
```

The wrapper destroys the genuine engine exactly once, including when wrapper
construction fails. The caller must keep the genuine plugin loaded until engine
destruction completes. Null initialization parameters, a null engine,
a null destroy callback, and inconsistent backend identity or
parameters are rejected with `std::invalid_argument`.
A null parameter map is accepted as an empty map, matching the backend interface.

All calls preserve the original arguments and return values. Output references
are passed directly to the genuine engine. Metadata, request handles, and memory
views remain genuine-backend objects; the wrapper does not replace or free them.
Call the corresponding release methods as usual before destroying the engine.

## Nonvirtual state and limitations

The base class exposes initialization state through nonvirtual getters. The
wrapper preserves the genuine engine's initialization error, backend type, and
parameter map, including parameters discovered during engine initialization.
These are snapshots at construction, not live views of later backend changes.
Use the same initialization parameters for both engines.

Telemetry-enabled initialization is accepted, so `NIXL_TELEMETRY_ENABLE` and
`captureTelemetry` deployments behave as they do without the wrapper. The
backend telemetry getter is nonvirtual and returns the wrapper's own queue, which
stays empty. No backend currently calls `addTelemetryEvent` and core does not
call `getTelemetryEvents`, so nothing is lost today. If backend telemetry comes
into use, forward it by draining the genuine engine's public
`getTelemetryEvents()` into the wrapper's queue.

This helper does not load shared libraries, preserve plugin-level identity by
itself, parse fault configuration, inject errors, or measure forwarding overhead.
Those remain subsequent steps. Functional forwarding tests do not establish the
design's statistical non-perturbation requirement.

## Validation

From the repository root, with NIXL build dependencies and Google Test/Mock:

```bash
meson setup build --buildtype=debugoptimized -Denable_plugins=POSIX -Dbuild_examples=false -Dwith_trace=false
meson compile -C build forwarding_engine_test
meson test -C build --no-rebuild forwarding_engine --print-errorlogs
```

The dedicated target needs no GPUs, network peers, or inference framework. Tests
exercise every current virtual method through a base-class reference, checking
call counts, argument identity, statuses, output parameters, opaque handles,
initialization state, and destruction. Status tests cover success, in-progress,
and all twelve errors. Add tests and overrides when the backend interface grows;
new pure virtual methods and changed overridden signatures also fail compilation.
