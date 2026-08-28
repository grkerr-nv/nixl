/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Test-only extension that hands test/python/mock_error_agent.py the same
 * error-injection engine and the same scenario table the C++ driver uses.
 * MOCK_BACKEND forwards southbound calls into an in-process GMock object, so
 * there is no way to drive it from Python without a shim like this one.
 */

#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "mocks/error_injection/error_injection.h"
#include "nixl_types.h"

namespace {

void
initGMockOnce() {
    static const bool initialized = []() {
        int argc = 1;
        char arg0[] = "nixl_gmock";
        char *argv[] = {arg0, nullptr};
        testing::InitGoogleMock(&argc, argv);
        return true;
    }();
    (void)initialized;
}

} // namespace

/**
 * @brief Holds a NiceMock error-injection engine for Python so MOCK_BACKEND can
 *        be told which southbound call to fail.
 *
 * Python has no equivalent of the C++ driver's member-ordering trick for
 * teardown, so the owner must drop the agent before dropping this object: agent
 * cleanup calls back into the engine.
 */
class pyGMockEngine {
public:
    pyGMockEngine() {
        initGMockOnce();
    }

    /* Fails `site` with `status` and counts every time the stub is reached. */
    void
    inject(const std::string &site, int status_value) {
        mocks::error_injection::applyInjection(engine_,
                                               mocks::error_injection::siteFromName(site),
                                               static_cast<nixl_status_t>(status_value),
                                               calls_);
    }

    /*
     * Same, for the scaffolding a scenario needs to reach its real injection
     * site (holding a transfer in NIXL_IN_PROG, say). Kept out of calls() so
     * that "the injected call was never reached" stays meaningful.
     */
    void
    stub(const std::string &site, int status_value) {
        mocks::error_injection::applyInjection(engine_,
                                               mocks::error_injection::siteFromName(site),
                                               static_cast<nixl_status_t>(status_value),
                                               stubCalls_);
    }

    unsigned
    calls() const {
        return calls_;
    }

    nixl_b_params_t
    params() const {
        nixl_b_params_t out;
        engine_.SetToParams(out);
        return out;
    }

private:
    /* Declared before the engine so the ON_CALL lambdas holding references to
     * these counters outlive nothing: the engine is destroyed first. */
    unsigned calls_ = 0;
    unsigned stubCalls_ = 0;
    testing::NiceMock<mocks::error_injection::mockErrorBackendEngine> engine_;
};

/* One row of the shared scenario table, flattened for Python. */
struct pyScenario {
    std::string name;
    std::string action;
    std::string site;
    int injected;
    int expected;
    std::string behavior;
};

PYBIND11_MODULE(nixl_gmock, m) {
    m.doc() = "Error-injection engine and scenario table for test/python/mock_error_agent.py";

    pybind11::class_<pyGMockEngine>(m, "GMockEngine")
        .def(pybind11::init<>())
        .def("inject", &pyGMockEngine::inject, pybind11::arg("site"), pybind11::arg("status"))
        .def("stub", &pyGMockEngine::stub, pybind11::arg("site"), pybind11::arg("status"))
        .def_property_readonly("calls", &pyGMockEngine::calls)
        .def("params", &pyGMockEngine::params);

    pybind11::class_<pyScenario>(m, "Scenario")
        .def_readonly("name", &pyScenario::name)
        .def_readonly("action", &pyScenario::action)
        .def_readonly("site", &pyScenario::site)
        .def_readonly("injected", &pyScenario::injected)
        .def_readonly("expected", &pyScenario::expected)
        .def_readonly("behavior", &pyScenario::behavior);

    m.def(
        "scenarios",
        []() {
            std::vector<pyScenario> out;
            for (const auto &s : mocks::error_injection::errorScenarios()) {
                out.push_back(pyScenario{s.name,
                                         mocks::error_injection::actionName(s.action),
                                         mocks::error_injection::siteName(s.site),
                                         static_cast<int>(s.injected),
                                         static_cast<int>(s.expected),
                                         mocks::error_injection::behaviorName(s.behavior)});
            }
            return out;
        },
        "The same scenario table the C++ driver walks.");

    m.def("status_str", [](int status) {
        return nixlEnumStrings::statusStr(static_cast<nixl_status_t>(status));
    });

    /*
     * nixl._bindings only exports nixl_status_t up to NIXL_ERR_NOT_SUPPORTED, so
     * a Python caller cannot name the last three codes. Export the full set here
     * so the driver can inject and report them.
     */
    m.def("statuses", []() {
        std::map<std::string, int> out;
        for (int value = NIXL_ERR_NO_TELEMETRY; value <= NIXL_IN_PROG; ++value) {
            const auto status = static_cast<nixl_status_t>(value);
            out.emplace(nixlEnumStrings::statusStr(status), value);
        }
        return out;
    });
}
