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


backend, forwarder = map(source, sys.argv[1:])
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
sys.stdout.write(
    f"All {sum(expected.values())} backend virtual methods are forwarded.\n"
)
