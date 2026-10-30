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

# Fault configuration

`fault_config.h` provides `nixl::fault::faultConfig`, an owning, validated snapshot
of the schema-version-1 status-rule configuration.

## API

```cpp
#include "fault_config.h"

const auto config = nixl::fault::faultConfig::load("/absolute/path/to/fault.toml");
const auto &plugin_path = config.getRealPluginPath();
const auto &experiment_id = config.getExperimentId();
const auto &rules = config.getRules();
```

`parse` takes TOML text instead of a path. Both copy what they need, so the
source can change or disappear afterwards.

The eventual proxy selects the file through `NIXL_FAULT_CONFIG_FILE` and retains
one snapshot during initialization.

## Schema

```toml
[fault_injection]
schema_version = 1
experiment_id = "first-write-postxfer-001"
real_plugin_path = "/usr/lib/nixl/plugins/libplugin_UCX.so"

[[fault_injection.rules]]
id = "fail-first-post"
method = "postXfer"
operation = "NIXL_WRITE"
inject_on_occurrence = 1
status = "NIXL_ERR_BACKEND"
```

`schema_version`, `experiment_id`, and `real_plugin_path` are required.
`schema_version` must be integer `1`. IDs and paths must be nonempty and free of
control characters, which would corrupt error messages and logs.
`real_plugin_path` must be absolute and name a file, not a directory; the loader
checks existence and plugin validity.

`rules` may be omitted or `[]` for an inert configuration. Each rule requires a
unique `id`, `method`, a positive `inject_on_occurrence`, and `status`.

Rules must not overlap. Two rules naming the same `method` and
`inject_on_occurrence` are rejected when their operations intersect, so at most
one rule matches any call and the evaluator needs no precedence order. Rules on
distinct methods, occurrences, or transfer directions remain independent.

Each rule targets exactly one occurrence. Schema version 1 cannot express
repeated, counted, or probabilistic injection, or selectors beyond `method` and
`operation`. A later schema may add them as optional rule keys, which this parser
rejects as unknown today.

`method` names a status-returning `nixlBackendEngine` virtual method:

- `registerMem`, `deregisterMem`, `connect`, `disconnect`, `unloadMD`.
- `prepXfer`, `postXfer`, `checkXfer`, `releaseReqH`, `prepMemView`.
- `getPublicData`, `getConnInfo`, `loadRemoteConnInfo`, `loadRemoteMD`, `loadLocalMD`.
- `getNotifs`, `genNotif`, `queryMem`, `estimateXferCost`.

`prepMemView` selects both overloads. Optional `operation` accepts `NIXL_READ` or
`NIXL_WRITE`, and only for the methods with an explicit operation argument:
`prepXfer`, `postXfer`, and `estimateXferCost`. An omitted `operation` matches
both transfer directions.

`status` accepts every symbolic `nixl_status_t` value: all 12 error statuses,
`NIXL_SUCCESS`, and `NIXL_IN_PROG`. Numeric statuses are rejected, as are unknown
keys at the document, section, or rule level.

## Errors

Schema violations raise `std::invalid_argument` naming the field and rule index;
invalid TOML raises it with the parser's source location. `load` raises
`std::runtime_error` when the path is not a regular file or cannot be opened or
read, and prepends the path to schema and TOML errors.

## Scope and validation

This component represents status rules only. Request-handle corruption fields
are rejected until their schema is defined. An inert consumer must reject
nonempty rules until it can execute them.

Tests extend the existing `plugin_loader_test` target and reuse its GoogleTest setup:

```bash
meson test -C build plugin_loader --test-args='--gtest_filter=faultConfig*.*' --print-errorlogs
```
