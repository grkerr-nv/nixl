#!/usr/bin/env python3

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

"""Runs the C++ driver's scenarios against the Python API instead of the C++ one.

test/gtest/mock_error_agent.cpp injects a southbound MOCK_BACKEND failure and
reports the nixl_status_t the C++ agent returned. This driver walks the same
scenario table -- literally the same table, read from the nixl_gmock extension --
through nixl_agent, and reports what a Python caller sees instead. The Python API
has no status codes: the bindings turn each of the 12 error codes into its own
exception type, and _api.py converts some of those into strings, bools, or
nothing at all. The divergence column says whether what Python surfaced still
identifies the status the C++ agent returned.

Differences between the two APIs that came out of running this, none of which
the C++ driver can see:

  * nixl_agent.send_notif() raises TypeError whenever its optional `backend`
    argument is given: _api.py passes the handle to genNotif as a bare int where
    the binding takes a list. Only the no-backend call path works.
  * nixl._bindings exports nixl_status_t only up to NIXL_ERR_NOT_SUPPORTED, so
    NIXL_ERR_REMOTE_DISCONNECT, NIXL_ERR_CANCELED and NIXL_ERR_NO_TELEMETRY
    cannot be named from Python even though all three have exception types.
  * transfer() and check_xfer_state() document an "ERR" return that cannot
    happen, because the bindings raise before _api.py inspects the status.
  * remove_remote_agent() discards invalidateRemoteMD's status entirely, and
    that binding is one of only two on nixlAgent bound without
    throw_nixl_exception, so it returns its status instead of raising. Neither
    mechanism for reporting the failure survives: NIXL_ERR_NOT_FOUND (unknown
    peer, or a second removal) and NIXL_ERR_INVALID_PARAM (the agent's own
    name) are reachable from any caller and vanish silently. Southbound
    injection cannot produce them, since they come from the agent's own
    bookkeeping rather than a backend, so the last section of the output
    demonstrates them directly instead.

Pass --csv for spreadsheet-friendly output, --list for the scenario names
accepted as a filter argument.

Run it from the build tree; nixl need not be pip-installed:

    LD_LIBRARY_PATH=... python3 test/python/mock_error_agent.py
"""

from __future__ import annotations

import gc
import importlib.util
import os
import sys
import types
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

import numpy as np

_REPO = Path(__file__).resolve().parents[2]
_GTEST_BUILD = _REPO / "build" / "test" / "gtest"


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {name} from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _bootstrap_uninstalled_nixl() -> None:
    """Make `import nixl` work from this repo's build tree when it is not pip-installed."""
    # Prefer an installed NIXL package. If it is unavailable, continue below
    # and assemble the package from this checkout's Python sources and built extensions.
    try:
        import nixl  # noqa: F401

        return
    except ImportError:
        pass

    bind_dir = _REPO / "build" / "src" / "bindings" / "python"
    bindings_so = next(bind_dir.glob("_bindings.cpython-*.so"), None)
    utils_so = next(bind_dir.glob("_utils.cpython-*.so"), None)
    api_dir = _REPO / "src" / "api" / "python"
    if bindings_so is None or utils_so is None or not api_dir.is_dir():
        raise ImportError(
            "nixl is not installed and the uninstalled Python bindings were not found. "
            "Build src/bindings/python/_bindings*.so or pip-install nixl."
        )

    # The wheel installs these modules under a CUDA-suffixed package name and
    # _api.py imports them by that name, so stand up the same package layout.
    cu_name = "nixl_cu12"
    pkg = types.ModuleType(cu_name)
    pkg.__path__ = [str(bind_dir), str(api_dir)]
    sys.modules[cu_name] = pkg
    _load_module(f"{cu_name}._bindings", bindings_so)
    _load_module(f"{cu_name}._utils", utils_so)
    _load_module(f"{cu_name}.logging", api_dir / "logging.py")
    _load_module(f"{cu_name}._api", api_dir / "_api.py")

    meta = _REPO / "src" / "bindings" / "python" / "nixl-meta"
    if str(meta) not in sys.path:
        sys.path.insert(0, str(meta))


def _quiet_nixl_logging() -> None:
    """Keep NIXL's per-agent INFO lines from interleaving with the table.

    Has to run before _api.py is imported, which is when NIXL reads this.
    """
    os.environ.setdefault("NIXL_LOG_LEVEL", "WARN")


_quiet_nixl_logging()
_bootstrap_uninstalled_nixl()

if _GTEST_BUILD.is_dir() and str(_GTEST_BUILD) not in sys.path:
    sys.path.insert(0, str(_GTEST_BUILD))

import nixl_gmock  # noqa: E402

from nixl._api import nixl_agent, nixl_agent_config  # noqa: E402
from nixl._bindings import (  # noqa: E402
    nixlBackendError,
    nixlCancelledError,
    nixlInvalidParamError,
    nixlMismatchError,
    nixlNotAllowedError,
    nixlNoTelemetryError,
    nixlNotFoundError,
    nixlNotPostedError,
    nixlNotSupportedError,
    nixlRemoteDisconnectError,
    nixlRepostActiveError,
    nixlUnknownError,
)

MOCK_BACKEND = "MOCK_BACKEND"
BUF_LEN = 256
NONE_SITE = "(none)"
MEM_TYPE = "DRAM"

# nixl._bindings only exports nixl_status_t up to NIXL_ERR_NOT_SUPPORTED, so take
# the values from the extension, which reads them off the C++ enum directly.
_STATUS = nixl_gmock.statuses()
SUCCESS = _STATUS["NIXL_SUCCESS"]
IN_PROG = _STATUS["NIXL_IN_PROG"]

# The bindings' throw_nixl_exception gives each error code its own type, so the
# exception a caller catches is a lossless stand-in for the status. This is that
# mapping, inverted.
_EXCEPTION_STATUS = {
    nixlNotPostedError: _STATUS["NIXL_ERR_NOT_POSTED"],
    nixlInvalidParamError: _STATUS["NIXL_ERR_INVALID_PARAM"],
    nixlBackendError: _STATUS["NIXL_ERR_BACKEND"],
    nixlNotFoundError: _STATUS["NIXL_ERR_NOT_FOUND"],
    nixlMismatchError: _STATUS["NIXL_ERR_MISMATCH"],
    nixlNotAllowedError: _STATUS["NIXL_ERR_NOT_ALLOWED"],
    nixlRepostActiveError: _STATUS["NIXL_ERR_REPOST_ACTIVE"],
    nixlUnknownError: _STATUS["NIXL_ERR_UNKNOWN"],
    nixlNotSupportedError: _STATUS["NIXL_ERR_NOT_SUPPORTED"],
    nixlRemoteDisconnectError: _STATUS["NIXL_ERR_REMOTE_DISCONNECT"],
    nixlCancelledError: _STATUS["NIXL_ERR_CANCELED"],
    nixlNoTelemetryError: _STATUS["NIXL_ERR_NO_TELEMETRY"],
}


def status_str(status: Optional[int]) -> str:
    return "(none reported)" if status is None else nixl_gmock.status_str(status)


@dataclass
class Observation:
    """What the Python API handed back for the call a scenario was aiming at.

    `status` is the nixl_status_t that the Python surface still identifies, or
    None when the surface cannot express one -- the interesting case, since the
    C++ agent always returns a status there.
    """

    stage: str
    surfaced: str
    status: Optional[int]
    value: object = None
    note: str = ""
    failure: str = ""


class MockAgent:
    """One Python agent plus the GMock engine backing its MOCK_BACKEND instance."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.gmock = nixl_gmock.GMockEngine()
        # Match nixlAgentConfig{} in the C++ driver: no progress thread, no
        # listener thread, telemetry off (control.telemetryDisabled needs that).
        # Python defaults enable_prog_thread to True, so say so explicitly.
        self.agent = nixl_agent(
            name,
            nixl_agent_config(
                enable_prog_thread=False,
                enable_listen_thread=False,
                capture_telemetry=False,
                backends=[],
            ),
        )

    @property
    def binding(self):
        """The pybind11 agent underneath, for statuses _api.py does not pass on."""
        return self.agent.agent

    def inject(self, site: str, status: int) -> None:
        self.gmock.inject(site, status)

    def stub(self, site: str, status: int) -> None:
        self.gmock.stub(site, status)

    def create_backend(self) -> None:
        self.agent.create_backend(MOCK_BACKEND, dict(self.gmock.params()))

    def close(self) -> int:
        """Tears the agent down and returns how often the injected stub was hit."""
        calls = self.gmock.calls
        # The agent calls back into the engine while tearing down, so it has to
        # go first. The C++ driver gets this ordering from member declaration
        # order; here it takes an explicit drop plus a collection for cycles.
        self.agent = None
        gc.collect()
        self.gmock = None
        return calls


class _Unhandled(Exception):
    """An exception from the Python API that is not one of the 12 typed errors."""


def _describe(value: object) -> str:
    if value is None:
        return "returned None"
    if isinstance(value, bool):
        return f"returned {value}"
    if isinstance(value, str):
        return f'returned "{value}"'
    return "returned normally"


Interpreter = Callable[[object], "tuple[str, Optional[int], str]"]


def _observe(
    stage: str, call: Callable[[], object], interpret: Optional[Interpreter] = None
) -> Observation:
    """Run the call a scenario is aiming at and record what Python surfaced.

    `interpret` turns a clean (non-raising) return into text, the status that
    return still identifies, and a note. The default reads any clean return as
    NIXL_SUCCESS, which is what a method that only raises on error means.
    """
    try:
        value = call()
    except Exception as exc:
        status = _EXCEPTION_STATUS.get(type(exc))
        if status is None:
            raise _Unhandled(f"{type(exc).__name__}: {exc}") from exc
        return Observation(stage, f"raised {type(exc).__name__}", status)

    if interpret is None:
        return Observation(stage, _describe(value), SUCCESS, value)
    text, status, note = interpret(value)
    return Observation(stage, text, status, value, note)


def _xfer_state(value: object) -> tuple[str, Optional[int], str]:
    """transfer() and check_xfer_state() collapse the status into a string."""
    if value == "DONE":
        return _describe(value), SUCCESS, ""
    if value == "PROC":
        return _describe(value), IN_PROG, ""
    # Unreachable: the bindings raise before _api.py can return "ERR". Reported
    # rather than assumed away, so the day it does happen it shows up here.
    return _describe(value), None, '"ERR" is documented but unreachable'


def _status_discarded(value: object) -> tuple[str, Optional[int], str]:
    """remove_remote_agent() drops invalidateRemoteMD's status on the floor.

    Nothing is lost on the injected path: the agent ignores the backend's
    disconnect() status itself, so NIXL_SUCCESS is what C++ reports here too.
    swallowed_statuses() covers the calls where this does lose a status.
    """
    return _describe(value), None, "status is always discarded"


def _check_side_effect(scenario, agent, remote_name, remote_descs) -> str:
    """PASS_THROUGH_INVALIDATED also claims the peer's metadata was dropped.

    The C++ driver proves it by reading NIXL_ERR_NOT_FOUND back from
    checkRemoteMD. Python only gets a bool here, so this can confirm the metadata
    is gone but not that NIXL_ERR_NOT_FOUND is the reason.
    """
    if scenario.behavior != "PASS_THROUGH_INVALIDATED":
        return ""
    if not agent.check_remote_metadata(remote_name, remote_descs):
        return ""
    # Kept free of commas so that --csv output stays parsable.
    return "remote metadata still resolvable"


def _run_scenario_body(scenario, local: MockAgent, remote: MockAgent) -> Observation:
    """Runs enough of the northbound sequence to reach the scenario's API.

    Mirrors runScenarioBody in test/gtest/mock_error_agent.cpp step for step.
    """
    action = scenario.action
    site = scenario.site
    injected = scenario.injected

    if action == "CREATE_BACKEND":
        local.inject(site, injected)

    obs = _observe("createBackend", local.create_backend)
    if obs.status != SUCCESS or action == "CREATE_BACKEND":
        return obs
    obs = _observe("createBackend (peer)", remote.create_backend)
    if obs.status != SUCCESS:
        return obs

    local_buf = np.full(BUF_LEN, 0xBB, dtype=np.uint8)
    remote_buf = np.zeros(BUF_LEN, dtype=np.uint8)
    local_addr = int(local_buf.ctypes.data)
    remote_addr = int(remote_buf.ctypes.data)

    local_reg = [(local_addr, BUF_LEN, 0, "")]
    remote_reg = [(remote_addr, BUF_LEN, 0, "")]

    if action == "REGISTER_MEM":
        local.inject(site, injected)

    obs = _observe(
        "registerMem",
        lambda: local.agent.register_memory(
            local_reg, mem_type=MEM_TYPE, backends=[MOCK_BACKEND]
        ),
    )
    if obs.status != SUCCESS:
        return obs
    local_reg_descs = obs.value

    obs = _observe(
        "registerMem (peer)",
        lambda: remote.agent.register_memory(
            remote_reg, mem_type=MEM_TYPE, backends=[MOCK_BACKEND]
        ),
    )
    if obs.status != SUCCESS:
        return obs

    if action == "DEREGISTER_MEM":
        local.inject(site, injected)
        return _observe(
            "deregisterMem",
            lambda: local.agent.deregister_memory(
                local_reg_descs, backends=[MOCK_BACKEND]
            ),
        )

    obs = _observe("getLocalMD (peer)", remote.agent.get_agent_metadata)
    if obs.status != SUCCESS:
        return obs
    remote_md = obs.value

    if action == "LOAD_REMOTE_MD":
        local.inject(site, injected)
    obs = _observe("loadRemoteMD", lambda: local.agent.add_remote_agent(remote_md))
    if obs.status != SUCCESS or action == "LOAD_REMOTE_MD":
        return obs
    remote_name = obs.value
    if isinstance(remote_name, bytes):
        remote_name = remote_name.decode()

    if action == "MAKE_CONNECTION":
        local.inject(site, injected)
        return _observe(
            "makeConnection",
            lambda: local.agent.make_connection(remote_name, backends=[MOCK_BACKEND]),
        )

    if action == "INVALIDATE_REMOTE_MD":
        local.inject(site, injected)
        return _observe(
            "invalidateRemoteMD",
            lambda: local.agent.remove_remote_agent(remote_name),
            _status_discarded,
        )

    if action == "GET_NOTIFS":
        local.inject(site, injected)
        return _observe(
            "getNotifs", lambda: local.agent.get_new_notifs(backends=[MOCK_BACKEND])
        )

    if action == "GEN_NOTIF":
        local.inject(site, injected)
        # No backend= argument: send_notif hands the handle to genNotif as a bare
        # int where the binding wants a list, so naming a backend always raises
        # TypeError (see this module's docstring). MOCK_BACKEND is the only
        # backend on this agent, so omitting it still reaches the mock.
        return _observe(
            "genNotif",
            lambda: local.agent.send_notif(remote_name, b"mock notification"),
        )

    if action == "QUERY_MEM":
        local.inject(site, injected)
        return _observe(
            "queryMem",
            lambda: local.agent.query_memory(
                local_reg, MOCK_BACKEND, mem_type=MEM_TYPE
            ),
        )

    if action == "PREP_REMOTE_MEM_VIEW":
        local.inject(site, injected)
        remote_view = local.agent.get_remote_descs(
            [(remote_addr, BUF_LEN, 0, remote_name)], mem_type=MEM_TYPE
        )
        obs = _observe(
            "prepMemView(remote)",
            lambda: local.agent.prep_mem_view(remote_view, backends=[MOCK_BACKEND]),
        )
        if obs.status == SUCCESS:
            local.agent.release_mem_view(obs.value)
        return obs

    if action == "PREP_LOCAL_MEM_VIEW":
        local.inject(site, injected)
        local_view = local.agent.get_xfer_descs(
            [(local_addr, BUF_LEN, 0)], mem_type=MEM_TYPE
        )
        obs = _observe(
            "prepMemView(local)",
            lambda: local.agent.prep_mem_view(local_view, backends=[MOCK_BACKEND]),
        )
        if obs.status == SUCCESS:
            local.agent.release_mem_view(obs.value)
        return obs

    src_dlist = local.agent.get_xfer_descs(
        [(local_addr, BUF_LEN, 0)], mem_type=MEM_TYPE
    )
    dst_dlist = local.agent.get_xfer_descs(
        [(remote_addr, BUF_LEN, 0)], mem_type=MEM_TYPE
    )

    if action == "CREATE_XFER":
        local.inject(site, injected)
    obs = _observe(
        "createXferReq",
        lambda: local.agent.initialize_xfer(
            "WRITE", src_dlist, dst_dlist, remote_name, backends=[MOCK_BACKEND]
        ),
    )
    if obs.status != SUCCESS:
        return obs
    req = obs.value

    try:
        if action == "STATUS_BEFORE_POST":
            return _observe(
                "getXferStatus", lambda: local.agent.check_xfer_state(req), _xfer_state
            )

        if action == "ESTIMATE_XFER_COST":
            local.inject(site, injected)
            return _observe(
                "estimateXferCost", lambda: local.agent.estimate_xfer_cost(req)
            )

        if action == "POST_XFER":
            local.inject(site, injected)
        elif action == "CHECK_XFER":
            local.stub("postXfer", IN_PROG)
            local.inject(site, injected)
        elif action == "RELEASE_XFER":
            local.stub("postXfer", IN_PROG)
            local.stub("checkXfer", IN_PROG)
            local.inject(site, injected)
        elif action == "POLL_TO_COMPLETION":
            local.inject(site, injected)
        elif action == "REPOST_ACTIVE":
            local.inject(site, injected)
            local.stub("checkXfer", IN_PROG)

        obs = _observe("postXferReq", lambda: local.agent.transfer(req), _xfer_state)
        if obs.status is None or obs.status < 0:
            obs.failure = _check_side_effect(
                scenario, local.agent, remote_name, dst_dlist
            )
            return obs

        if action == "POST_XFER":
            return obs

        if action == "CHECK_XFER":
            obs = _observe(
                "getXferStatus", lambda: local.agent.check_xfer_state(req), _xfer_state
            )
            obs.failure = _check_side_effect(
                scenario, local.agent, remote_name, dst_dlist
            )
            return obs

        if action == "RELEASE_XFER":
            # release() only marks the handle released once the underlying call
            # succeeds, so the retry in the finally below frees it, exactly as
            # the C++ driver's second releaseXferReq does.
            return _observe(
                "releaseXferReq", lambda: local.agent.release_xfer_handle(req)
            )

        if action == "REPOST_ACTIVE":
            return _observe(
                "postXferReq(repost)", lambda: local.agent.transfer(req), _xfer_state
            )

        while obs.status == IN_PROG:
            obs = _observe(
                "getXferStatus", lambda: local.agent.check_xfer_state(req), _xfer_state
            )
        if obs.status != SUCCESS:
            return obs

        if action == "TELEMETRY_DISABLED":
            return _observe(
                "getXferTelemetry", lambda: local.agent.get_xfer_telemetry(req)
            )

        return Observation("completed", "completed", SUCCESS)
    finally:
        try:
            local.agent.release_xfer_handle(req)
        except Exception:
            # RELEASE_XFER deliberately leaves a handle whose release fails; the
            # agent's own teardown retries it from its leaked-handle list.
            pass


def run_scenario(scenario) -> Observation:
    """Sets up and tears down fresh agents, and checks the site was reached.

    An injected error that is never returned to the agent looks exactly like one
    the agent deliberately discarded, so report it rather than pass silently.
    """
    local = MockAgent("Agent001")
    remote = MockAgent("Agent002")
    obs = None
    try:
        obs = _run_scenario_body(scenario, local, remote)
    except _Unhandled as exc:
        obs = Observation("(driver)", "unhandled", None, failure=str(exc))
    finally:
        calls = local.close()
        remote.close()

    if scenario.site != NONE_SITE and calls == 0 and not obs.failure:
        obs.failure = "injected call was never reached"
    return obs


def divergence(scenario, obs: Observation) -> str:
    """Whether what Python surfaced still identifies the C++ agent's status."""
    if obs.failure:
        return "FAIL: " + obs.failure
    if obs.status == scenario.expected:
        return "-"
    if obs.status is None:
        # A silent return is the right signal for success; it is only a loss
        # when there was a status worth hearing about.
        if scenario.expected == SUCCESS:
            return "-"
        return "lossy: cannot tell " + status_str(scenario.expected)
    return "differs: python implies " + status_str(obs.status)


# Failures the scenario table structurally cannot show. Their statuses come from
# the agent's own bookkeeping rather than from a backend, so injecting southbound
# errors never produces them, yet any caller can hit them.
_SWALLOWED_CALLS = (
    ("unknown peer", "NoSuchPeer"),
    ("the agent's own name", ""),
)


def swallowed_statuses() -> list[str]:
    """Calls where _api.py drops a status the binding had already handed it.

    invalidateRemoteMD returns its status rather than raising, and
    remove_remote_agent() ignores the return, so the failure reaches Python and
    stops there. Make each call both ways to show the status existing and then
    not.
    """
    agent = MockAgent("SwallowProbe")
    reports = []
    try:
        for label, peer in _SWALLOWED_CALLS:
            target = peer or agent.name
            surfaced = _describe(agent.agent.remove_remote_agent(target))
            status = int(agent.binding.invalidateRemoteMD(target))
            if status == SUCCESS:
                continue
            reports.append(
                f"remove_remote_agent, {label}: {surfaced}, "
                f"binding had {status_str(status)}"
            )
    finally:
        agent.close()
    return reports


_COLUMN_WIDTH = 30
_COLUMN_GAP = 4
_HEADERS = (
    "sb site",
    "injected",
    "python stopped at",
    "c++ returned",
    "python surfaced",
    "divergence",
)


def column(value: str) -> str:
    """Pads to a fixed column, never below _COLUMN_GAP trailing spaces."""
    padding = (
        _COLUMN_WIDTH - len(value)
        if _COLUMN_WIDTH > len(value) + _COLUMN_GAP
        else _COLUMN_GAP
    )
    return value + " " * padding


def _row(cells: tuple[str, ...]) -> str:
    return "".join(column(c) for c in cells[:-1]) + cells[-1]


def _ensure_plugin_dir() -> None:
    if os.environ.get("NIXL_PLUGIN_DIR"):
        return
    plugin_dir = Path(nixl_gmock.__file__).resolve().parent / "mocks"
    if plugin_dir.is_dir():
        os.environ["NIXL_PLUGIN_DIR"] = str(plugin_dir)


def main(argv: list[str]) -> int:
    _ensure_plugin_dir()

    csv = False
    listing = False
    name_filter = ""
    for arg in argv:
        if arg == "--csv":
            csv = True
        elif arg == "--list":
            listing = True
        elif not name_filter and not arg.startswith("--"):
            name_filter = arg
        else:
            print(
                f"Usage: {sys.argv[0]} [--csv] [--list] [scenario-name|injection-site]",
                file=sys.stderr,
            )
            return 2

    scenarios = nixl_gmock.scenarios()

    if listing:
        for scenario in scenarios:
            print(scenario.name)
        return 0

    if csv:
        print(",".join(_HEADERS))
    else:
        print(f"Injecting southbound errors into {MOCK_BACKEND} via the Python API\n")
        print(_row(_HEADERS))
        print("-" * (_COLUMN_WIDTH * (len(_HEADERS) - 1) + 40))

    selected = 0
    failures = 0
    notes: list[tuple[str, str]] = []
    for scenario in scenarios:
        if name_filter and name_filter not in (scenario.name, scenario.site):
            continue
        selected += 1

        obs = run_scenario(scenario)
        verdict = divergence(scenario, obs)
        if obs.failure:
            failures += 1
        if obs.note:
            notes.append((scenario.name, obs.note))

        cells = (
            scenario.site,
            "-" if scenario.site == NONE_SITE else status_str(scenario.injected),
            obs.stage,
            status_str(scenario.expected),
            obs.surfaced,
            verdict,
        )
        print(",".join(cells) if csv else _row(cells))

    if selected == 0:
        print(f"No scenario or injection site matches '{name_filter}'", file=sys.stderr)
        return 2

    if not csv:
        print(f"\n{selected} scenarios, {failures} failures")
        if notes:
            print("\nWhere the Python surface cannot carry a status:")
            for name, note in notes:
                print(f"  {name}: {note}")
        if not name_filter:
            swallowed = swallowed_statuses()
            if swallowed:
                print("\nStatuses no scenario can reach, dropped before the caller:")
                for report in swallowed:
                    print(f"  {report}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
