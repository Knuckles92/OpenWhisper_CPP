// Golden vectors of ui_qt.dialogs.settings_remote's wording helpers
// (tools/parity/cases/ui_qt.dialogs.settings_remote.py) and the tests that pin them by hand.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "net/tailscale.h"
#include "pyport/errors.h"
#include "remote/control.h"
#include "remote/record_sync_control.h"
#include "ui_remote/remote_text.h"

namespace golden = fv::golden;
using fv::Json;
namespace text = fv::remote_text;

namespace {

constexpr std::string_view kModule = "ui_qt.dialogs.settings_remote";

// The vectors were made on a machine in Pacific time (pins.local_timezone).
const std::chrono::time_zone& pacific() {
    return *std::chrono::locate_zone("America/Los_Angeles");
}

std::string str(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool flag(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_boolean() && found->get<bool>();
}

std::optional<bool> maybe(const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || found->is_null()) {
        return std::nullopt;
    }
    return found->get<bool>();
}

fv::remote::SyncStatus syncStatus(const Json& fields) {
    fv::remote::SyncStatus status;
    status.location = str(fields, "location");
    status.paired = flag(fields, "paired");
    status.host_name = str(fields, "host_name");
    status.host_keeps = maybe(fields, "host_keeps");
    status.host_supports = maybe(fields, "host_supports");
    return status;
}

fv::remote::HostState hostState(const Json& fields) {
    fv::remote::HostState state;
    state.enabled = flag(fields, "enabled");
    state.running = flag(fields, "running");
    state.port = fields.at("port").get<int>();
    state.error = str(fields, "error");
    state.host_name = str(fields, "host_name");
    if (fields.at("address").is_string()) {
        state.address = fields.at("address").get<std::string>();
    }
    if (fields.at("address_kind").is_string()) {
        state.address_kind = fields.at("address_kind").get<std::string>();
    }
    state.fingerprint = str(fields, "fingerprint");
    state.engine = fields.at("engine");
    return state;
}

std::optional<fv::net::TailscaleStatus> tailnet(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    fv::net::TailscaleStatus status;
    status.state = str(fields, "state");
    status.name = str(fields, "name");
    status.address = str(fields, "address");
    status.owner = str(fields, "owner");
    return status;
}

// The dict the case file stands for a NearbyHost with.
fv::remote::NearbyHost nearby(const Json& fields) {
    fv::remote::NearbyHost host;
    host.host_name = str(fields, "host_name");
    host.fingerprint = str(fields, "fingerprint");
    host.engine = fields.at("engine");
    host.compatible = flag(fields, "compatible");
    host.approval = flag(fields, "approval");
    if (fields.at("lan").is_string()) {
        fv::remote::LanHost lan;
        lan.address = fields.at("lan").get<std::string>();
        lan.port = 47821;
        lan.host_name = host.host_name;
        lan.fingerprint = host.fingerprint;
        lan.engine = host.engine;
        lan.protocol = host.compatible ? 1 : 2;
        lan.approval = host.approval;
        host.lan = lan;
    }
    if (fields.at("tailnet").is_object()) {
        const Json& peer = fields.at("tailnet");
        fv::remote::TailnetHost found;
        found.peer.name = str(peer, "name");
        found.peer.address = str(peer, "address");
        found.peer.owner = str(peer, "owner");
        found.peer.mine = flag(peer, "mine");
        found.port = 47821;
        found.host_name = host.host_name;
        found.engine = host.engine;
        found.tailscale_pairing = flag(peer, "pairing");
        found.compatible = host.compatible;
        found.fingerprint = host.fingerprint;
        found.approval = host.approval;
        host.tailnet = found;
    }
    return host;
}

} // namespace

TEST_CASE("golden ui_qt.dialogs.settings_remote _paired_on", "[golden]") {
    golden::for_each(kModule, "_paired_on", [](const golden::Case& c) {
        CHECK(text::paired_on(c.arg<Json>(0), pacific()) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _stored_phrase", "[golden]") {
    golden::for_each(kModule, "_stored_phrase", [](const golden::Case& c) {
        CHECK(text::stored_phrase(c.arg<Json>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _records_location_description", "[golden]") {
    golden::for_each(kModule, "_records_location_description", [](const golden::Case& c) {
        const auto location = c.arg<std::string>(0);
        const auto host = c.arg<std::string>(1);
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_THROWS_AS(text::records_location_description(location, host),
                            std::invalid_argument);
            return;
        }
        CHECK(text::records_location_description(location, host) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _records_host_problem", "[golden]") {
    golden::for_each(kModule, "_records_host_problem", [](const golden::Case& c) {
        CHECK(golden::equal_rebranded(
            c.result<std::string>(),
            text::records_host_problem(syncStatus(c.arg<Json>(0)), c.arg<std::string>(1))));
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _sharing_address", "[golden]") {
    golden::for_each(kModule, "_sharing_address", [](const golden::Case& c) {
        CHECK(text::sharing_address(hostState(c.arg<Json>(0)), tailnet(c.arg<Json>(1)),
                                    c.arg<bool>(2)) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _sharing_status", "[golden]") {
    golden::for_each(kModule, "_sharing_status", [](const golden::Case& c) {
        CHECK(text::sharing_status(hostState(c.arg<Json>(0)), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _engine_phrase", "[golden]") {
    golden::for_each(kModule, "_engine_phrase", [](const golden::Case& c) {
        CHECK(text::engine_phrase(c.arg<Json>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote _find_row_detail", "[golden]") {
    golden::for_each(kModule, "_find_row_detail", [](const golden::Case& c) {
        const Json fields = c.arg<Json>(0);
        const fv::remote::NearbyHost host = nearby(fields);
        const Json expected = c.result<Json>();
        CHECK(golden::equal_rebranded(expected.at("text").get<std::string>(),
                                      text::find_row_detail(host)));
        CHECK(text::find_row_connects(host) == (expected.at("button") == "Connect"));
        CHECK(host.address() == expected.at("tooltip").get<std::string>());
        // The row's button is enabled for a computer it can pair with while nothing pairs.
        const bool busy = fields.value("busy", false);
        CHECK((host.compatible && !busy) == expected.at("enabled").get<bool>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_remote pairing_grants_phrase", "[golden]") {
    golden::for_each(kModule, "pairing_grants_phrase", [](const golden::Case& c) {
        CHECK(text::pairing_grants_phrase(c.arg<std::vector<std::string>>(0)) ==
              c.result<std::string>());
    });
}

// from tests/test_remote_engine_push.py::test_share_line_names_the_compute_type
TEST_CASE("share line names the compute type", "[test_remote_engine_push]") {
    CHECK(text::engine_phrase(Json{
              {"label", "Whisper turbo"}, {"device", "cuda"}, {"compute_type", "int8_float32"}}) ==
          "Whisper turbo on cuda (int8_float32)");
    // Engines without one (and hosts from before) read as they did.
    CHECK(text::engine_phrase(Json{{"label", "Parakeet v3"}, {"device", "cpu"}}) ==
          "Parakeet v3 on cpu");
}

TEST_CASE("stored phrase of the record sync's counts", "[remote_text]") {
    std::map<std::string, fv::remote::StoredCount> stored;
    CHECK(text::stored_phrase(stored).empty());
    stored["dictation"] = {12, 1288490188};
    stored["meeting"] = {3, 0};
    CHECK(text::stored_phrase(stored) == "12 dictations and 3 meetings (1.2 GB)");
    stored["dictation"] = {1, 0};
    stored.erase("meeting");
    CHECK(text::stored_phrase(stored) == "1 dictation");
}

TEST_CASE("a count int() refuses raises ValueError", "[remote_text]") {
    CHECK_THROWS_AS(text::stored_phrase(Json{{"dictation", {{"count", "many"}}}}),
                    fv::py::ValueError);
}
