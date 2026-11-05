# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compare virtual names and overload counts; C++ override checks enforce signatures."""

import re
import sys
from collections import Counter
from pathlib import Path


def source(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", Path(path).read_text(), flags=re.S)


if len(sys.argv) != 4:
    sys.exit("Usage: check_forwarding_interface.py BACKEND_HEADER FORWARDER CONFIG")
backend, forwarder, config = map(source, sys.argv[1:])
expected = Counter(re.findall(r"\bvirtual\b[^;{}()]*?\b(\w+)\s*\(", backend))
expected.pop("nixlBackendEngine", None)  # The implicit derived destructor suffices.
actual = Counter(re.findall(r"\b(\w+)\s*\([^;{}]*?\)[^;{}]*?\boverride\b", forwarder))

# Fail closed if interface syntax grows beyond this deliberately small scanner.
if not expected or sum(expected.values()) + 1 != len(
    re.findall(r"\bvirtual\b", backend)
):
    sys.exit(
        "Cannot identify every backend virtual method; update the interface check."
    )
if expected != actual:
    sys.exit(
        f"Forwarding interface mismatch: missing {dict(expected - actual)}, "
        f"unexpected {dict(actual - expected)}. Update the forwarder and its tests."
    )
status_methods = Counter(re.findall(r"\bvirtual\s+nixl_status_t\s+(\w+)\s*\(", backend))
injected_methods = Counter(
    re.findall(
        r"\bnixl_status_t\s+(\w+)\s*\([^;{}]*\)[^;{}]*?\boverride\s*"
        r"\{\s*if\s*\(const\s+auto\s+status\s*=\s*injectedStatus\(",
        forwarder,
    )
)
if not status_methods or status_methods != injected_methods:
    sys.exit(
        f"Injection interface mismatch: missing {dict(status_methods - injected_methods)}, "
        f"unexpected {dict(injected_methods - status_methods)}. Update status interception."
    )
# The evaluator takes an operation only for the methods that carry one and asserts the
# pairing, but release builds define NDEBUG and drop that assert, so check it here where
# the build fails instead. A call site that passes std::nullopt for a transfer method
# stops matching every directional rule, and the run still looks clean.
carries_body = re.search(r"\bcarriesOperation\s*\([^)]*\)\s*\{(.*?)\}", config, flags=re.S)
if carries_body is None:
    sys.exit("Cannot find carriesOperation(); update the interface check.")
carries = set(re.findall(r"\bbackend_method_t::(\w+)", carries_body.group(1)))
call_sites = len(re.findall(r"\binjectedStatus\(\s*backend_method_t::", forwarder))
injections = re.findall(
    r"\binjectedStatus\(\s*backend_method_t::(\w+)\s*,\s*([^()]*?)\s*\)",
    forwarder,
    flags=re.S,
)
# Fail closed if a call site stops looking like the two plain arguments parsed here.
if not carries or not call_sites or len(injections) != call_sites:
    sys.exit("Cannot read every injection call site; update the interface check.")
mispaired = sorted(
    {
        f"{method} must pass the operation"
        if method in carries
        else f"{method} must pass std::nullopt"
        for method, argument in injections
        if (argument != "std::nullopt") != (method in carries)
    }
)
if mispaired:
    sys.exit(
        f"Operation pairing mismatch: {'; '.join(mispaired)}. The forwarder and "
        "carriesOperation() must agree, or the evaluator stops matching directional rules."
    )
sys.stdout.write(
    f"All {sum(expected.values())} backend virtual methods are forwarded; "
    f"{sum(status_methods.values())} status routes support injection; "
    f"{call_sites} call sites pair the operation correctly.\n"
)
