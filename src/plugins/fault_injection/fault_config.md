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
of the schema-version-1 status-rule configuration. `rule_evaluator.h` provides
`nixl::fault::ruleEvaluator`, which applies those rules to backend calls.

## API

### Configuration

```cpp
#include "fault_config.h"

const auto config = nixl::fault::faultConfig::load("/absolute/path/to/fault.toml");
const auto &plugin_path = config.getRealPluginPath();
const auto &experiment_id = config.getExperimentId();
const auto &rules = config.getRules();
```

`parse` takes TOML text instead of a path. Both copy what they need, so the
source can change or disappear afterwards.

The proxy loads the file through `NIXL_FAULT_CONFIG_FILE` once per initialized
lifetime. It uses `getRealPluginPath()` to load the genuine backend and rejects
nonempty rules until runtime injection is implemented.

### Rule evaluator

`rule_evaluator.h` provides `nixl::fault::ruleEvaluator`, which owns a copy of the
validated rules and their runtime counters:

```cpp
nixl::fault::ruleEvaluator evaluator(config);
const auto *rule = evaluator.evaluate(nixl::fault::backend_method_t::POST_XFER, NIXL_WRITE);
// nullptr means forward; otherwise rule->status is the configured result to inject.
```

Pass the operation the backend call carried, or `std::nullopt` for the methods
that take none; `carriesOperation` says which, and the two must agree. A missing
operation matches no rule and would report a clean run instead of the configured
fault, so the pairing is asserted rather than defaulted.

Each evaluator has independent counters. Only matching methods and operations
advance them, and all matching rules advance even when another rule fires on the
same call. Evaluation takes no lock: a call matching no rule reads only the
immutable definitions, and a spent rule costs one relaxed atomic load, so the
backend path pays nothing for rules that cannot fire. Matching rules count down
atomically, so exactly one caller can take a rule from 1 to 0 and select it.
Returned rule pointers remain valid until the evaluator is destroyed.

Counters live in the evaluator, so a second evaluator over the same configuration
restarts them. The proxy constructs one per loaded plugin and shares it with every
backend engine; one per engine would inject each rule once per engine instead of
once per run.

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

Omitting `operation` makes a rule count both reads and writes. Replace the rule
in the example above with:

```toml
[[fault_injection.rules]]
id = "fail-second-post"
method = "postXfer"
inject_on_occurrence = 2
status = "NIXL_ERR_BACKEND"
```

The evaluator selects this rule on the second `postXfer` call. For example, a read
followed by a write selects it on the write. Runtime injection is not wired into
the proxy yet, so the proxy still rejects configurations with nonempty rules.

The parser rejects rules that could fire on the same call. Each rule counts its
own matching calls, so different occurrence numbers can still conflict. For the
same method, a read followed by a write triggers both an any-direction rule at
occurrence 2 and a write-only rule at occurrence 1.

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

The proxy still rejects nonempty rules. Connecting this evaluator to backend
calls and logging injections is separate work.

This component represents status rules only. Request-handle corruption fields
are rejected until their schema is defined. An inert consumer must reject
nonempty rules until it can execute them.

Tests extend the existing `plugin_loader_test` target and reuse its GoogleTest
setup. The whole target runs in milliseconds, so run it unfiltered; a suite filter
only risks skipping the component you changed:

```bash
meson test -C build plugin_loader --print-errorlogs
```
