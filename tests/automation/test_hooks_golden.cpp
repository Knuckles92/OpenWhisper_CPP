#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "automation/detail/filter_verdict.h"
#include "automation/event_bus.h"
#include "automation/filters.h"
#include "automation/hook_http.h"
#include "automation/programs.h"
#include "automation/webhooks.h"
#include "base/identity.h"
#include "golden/golden.h"
#include "golden_support.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"

// Golden replay of services.automation.webhooks, .filters and .programs (M5.11): the vectors come
// from OpenWhisper's own functions through tools/parity/cases/services.automation.*.py.

using namespace fv::automation;
using fv::Json;
namespace golden = fv::golden;

namespace {

// The case file's NOW, 2026-11-13T12:00:00+00:00.
std::chrono::system_clock::time_point pinned_now(const std::string& iso) {
    REQUIRE(iso == "2026-11-13T12:00:00+00:00");
    return std::chrono::sys_days{std::chrono::year{2026} / 11 / 13} + std::chrono::hours{12};
}

std::string_view kind_name(detail::VerdictKind kind) {
    switch (kind) {
    case detail::VerdictKind::Keep:
        return "keep";
    case detail::VerdictKind::Replace:
        return "replace";
    case detail::VerdictKind::Cancel:
        return "cancel";
    case detail::VerdictKind::Failed:
        return "failed";
    case detail::VerdictKind::Canceled:
        return "canceled";
    }
    return "";
}

Json verdict_json(const detail::Verdict& verdict) {
    return Json::array({std::string(kind_name(verdict.kind)), verdict.text, verdict.reason});
}

std::string from_hex(const std::string& hex) {
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

// The fv_http failure each httpx exception class stands for.
fv::http::Error transport_error(const std::string& name) {
    using fv::http::ErrorKind;
    if (name == "ConnectError") {
        return {ErrorKind::Connect, "[WinError 10061] No connection could be made because the "
                                    "target machine actively refused it"};
    }
    if (name == "ProxyError") {
        return {ErrorKind::Protocol, "CONNECT tunnel failed, response 407"};
    }
    if (name == "RemoteProtocolError") {
        return {ErrorKind::Connect, "Remote end closed connection without response"};
    }
    if (name == "ReadError") {
        fv::http::Error error{ErrorKind::Connect,
                              "[WinError 10054] An existing connection was forcibly closed by "
                              "the remote host"};
        error.after_request = true;
        return error;
    }
    if (name == "WriteError") {
        return {ErrorKind::Connect, "[Errno 32] Broken pipe"};
    }
    FAIL("no stand-in for httpx." << name);
    return {};
}

} // namespace

TEST_CASE("golden: webhooks._retry_after", "[golden][webhooks]") {
    golden::for_each("services.automation.webhooks", "retry_after", [](const golden::Case& c) {
        const auto value = c.arg<std::optional<std::string>>(0);
        const auto now = pinned_now(c.arg<std::string>(1));
        const auto expected = c.result<std::optional<double>>();
        const auto actual = hook_http::retry_after(value, now);
        CHECK(actual == expected);
    });
}

TEST_CASE("golden: webhooks.no_answer_copy", "[golden][webhooks]") {
    golden::for_each("services.automation.webhooks", "no_answer_copy", [](const golden::Case& c) {
        CHECK(hook_http::no_answer_copy(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden: webhooks._transport_copy", "[golden][webhooks]") {
    golden::for_each("services.automation.webhooks", "transport_copy", [](const golden::Case& c) {
        CHECK(hook_http::transport_copy(transport_error(c.arg<std::string>(0))) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden: webhooks._signed_headers", "[golden][webhooks]") {
    golden::for_each("services.automation.webhooks", "signed_headers", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        try {
            const auto headers = signed_headers(c.arg<std::string>(0), c.arg<std::string>(1),
                                                c.arg<std::string>(2), c.arg<std::int64_t>(3));
            Json actual = Json::array();
            for (const auto& [name, value] : headers) {
                actual.push_back(Json::array({name, value}));
            }
            CHECK(golden::equal(expected, actual));
        } catch (const fv::py::ValueError& error) {
            CHECK(golden::equal(expected, Json{{"refused", error.what()}}));
        }
    });
}

TEST_CASE("golden: filters._interpret", "[golden][filters]") {
    golden::for_each("services.automation.filters", "interpret", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            verdict_json(detail::interpret(c.arg<std::string>(0)))));
    });
    golden::for_each("services.automation.filters", "interpret_hex", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            verdict_json(detail::interpret(from_hex(c.arg<std::string>(0))))));
    });
}

TEST_CASE("golden: filters._lease_timeout and _lease_modes through register_lease",
          "[golden][filters]") {
    golden::for_each("services.automation.filters", "lease_timeout", [](const golden::Case& c) {
        FilterLeases leases;
        const Json expected = c.result<Json>();
        try {
            const Lease lease =
                leases.register_lease("http://127.0.0.1:9/f", "before_cleanup", c.arg<Json>(0));
            CHECK(golden::equal(expected, Json(lease.timeout_ms)));
        } catch (const fv::py::ValueError& error) {
            CHECK(golden::equal(expected, Json{{"refused", error.what()}}));
        }
    });
    golden::for_each("services.automation.filters", "lease_modes", [](const golden::Case& c) {
        FilterLeases leases;
        const Json expected = c.result<Json>();
        try {
            const Lease lease = leases.register_lease("http://127.0.0.1:9/f", "before_cleanup",
                                                      std::nullopt, c.arg<Json>(0));
            CHECK(golden::equal(expected, Json(lease.modes)));
        } catch (const fv::py::ValueError& error) {
            CHECK(golden::equal(expected, Json{{"refused", error.what()}}));
        }
    });
}

TEST_CASE("golden: filters.register_lease", "[golden][filters]") {
    golden::for_each("services.automation.filters", "register_lease", [](const golden::Case& c) {
        FilterLeases leases;
        Json options = c.arg<Json>(2);
        if (options.value("full", false)) {
            for (std::size_t i = 0; i < filters::MAX_LEASES; ++i) {
                leases.register_lease("http://127.0.0.1:9/f", "before_cleanup");
            }
        }
        std::optional<Json> timeout;
        std::optional<Json> modes;
        if (options.contains("timeout_ms")) {
            timeout = options["timeout_ms"];
        }
        if (options.contains("modes")) {
            modes = options["modes"];
        }
        const Json expected = c.result<Json>();
        try {
            const Lease lease =
                leases.register_lease(c.arg<std::string>(0), c.arg<std::string>(1), timeout, modes);
            CHECK(golden::equal(expected, Json::array({lease.url, lease.stage, lease.timeout_ms,
                                                       Json(lease.modes)})));
        } catch (const fv::py::ValueError& error) {
            CHECK(golden::equal(expected, Json{{"refused", error.what()}}));
        }
    });
}

TEST_CASE("golden: programs._run_event", "[golden][programs]") {
    golden::for_each("services.automation.programs", "run_event", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        Event event;
        event.id = c.arg<std::string>(3);
        event.seq = c.arg<std::int64_t>(4);
        event.type = c.arg<std::string>(2);
        event.time = "2026-01-01T00:00:00.000Z";
        event.data = Json{{"x", 1}};

        fv::RunOnceResult result;
        result.exit_code = c.arg<std::optional<int>>(0);
        result.error = c.arg<std::string>(1);
        const RunRecord record = run_record(event.id, "", result, 0);
        CHECK(record.event_id == expected["event_id"].get<std::string>());
        CHECK(record.ok == expected["ok"].get<bool>());
        CHECK(golden::equal(expected["exit_code"],
                            record.exit_code ? Json(*record.exit_code) : Json(nullptr)));
        CHECK(record.error == expected["error"].get<std::string>());
        CHECK(event.to_json() + "\n" == expected["stdin"].get<std::string>());

        // OpenWhisper sets its own names; OpenWhisper_CPP sets its own and those beside them.
        const auto env = event_environment(event);
        CHECK(env.size() == 2 * expected["env"].size());
        for (const auto& [name, value] : expected["env"].items()) {
            const std::string text = value.get<std::string>();
            CHECK(env.at(name) == text);
            CHECK(env.at(golden::rebrand(name)) == text);
        }
    });
}
