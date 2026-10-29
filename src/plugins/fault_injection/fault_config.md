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
of the schema-version-1 status-rule configuration. It reuses the existing toml++
dependency and does not load a plugin or evaluate rules.

## API

```cpp
#include "fault_config.h"

const auto config = nixl::fault::faultConfig::load("/absolute/path/to/fault.toml");
const auto &plugin_path = config.getRealPluginPath();
const auto &experiment_id = config.getExperimentId();
const auto &rules = config.getRules();
```

`faultConfig::parse(text)` parses TOML supplied as `std::string_view`.
`faultConfig::load(path)` reads a file and applies the same validation. Both return
an independent snapshot: changes to the source text or file do not change it.
Accessors return const references, valid while the configuration remains alive
and unchanged. Rule definitions contain typed backend methods, optional transfer
operations, one-based occurrence numbers, and NIXL statuses. Runtime counters and
spent-rule state belong to the evaluator, outside the parsed configuration.

The eventual proxy selects the file through `NIXL_FAULT_CONFIG_FILE` and retains
one snapshot during initialization. The parser itself does not read environment
variables or install process-wide state.

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
`schema_version` must be integer `1`. IDs and paths must be nonempty strings
without NUL bytes. `real_plugin_path` must be absolute; its existence and plugin
validity are checked by the loader, not by this parser.

`rules` may be omitted or set to `[]` for an inert configuration. Each rule
requires a unique `id`, `method`, `inject_on_occurrence`, and `status`.
`inject_on_occurrence` must be an integer from 1 through 9223372036854775807,
the upper bound of TOML's signed 64-bit integer range. File order is preserved.

`method` names a status-returning `nixlBackendEngine` virtual method:

- `registerMem`, `deregisterMem`, `connect`, `disconnect`, `unloadMD`.
- `prepXfer`, `postXfer`, `checkXfer`, `releaseReqH`, `prepMemView`.
- `getPublicData`, `getConnInfo`, `loadRemoteConnInfo`, `loadRemoteMD`, `loadLocalMD`.
- `getNotifs`, `genNotif`, `queryMem`, `estimateXferCost`.

`prepMemView` selects both overloads. Optional `operation` accepts `NIXL_READ` or
`NIXL_WRITE` only for methods with an explicit operation argument: `prepXfer`,
`postXfer`, and `estimateXferCost`. Methods receiving only an opaque request
handle do not expose an operation to this configuration model.

`status` accepts every current symbolic `nixl_status_t` value, including all
12 error statuses, `NIXL_SUCCESS`, and `NIXL_IN_PROG`. Numeric statuses are rejected.
Unknown keys at the document, section, or rule level are rejected. Invalid
field types, unknown symbols, duplicate rule IDs, and invalid occurrence numbers
raise `std::invalid_argument` with field and rule-index context. Invalid TOML
also raises `std::invalid_argument`, with the parser's source location. File
access failures raise `std::runtime_error`; schema errors loaded from a file
include its path.

## Scope and validation

This component represents status rules only. Request-handle corruption fields
are rejected until their schema is defined. It does not implement injection,
rule matching, configuration digests, or structured logging. An inert consumer
must reject nonempty rules until it can execute them.

Tests extend the existing `plugin_loader_test` target and reuse its GoogleTest
setup without adding plugin fixtures or dependencies:

```bash
meson test -C build plugin_loader --test-args='--gtest_filter=faultConfig*.*' --print-errorlogs
```
