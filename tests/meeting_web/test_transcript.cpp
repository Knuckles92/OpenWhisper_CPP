// Replays tests/fixtures/transcripts/meeting_ws.json, recorded from OpenWhisper's hub by
// tools/parity/transcripts/meeting_ws.py, against the C++ hub on a loopback server: the same
// engine (a real store with ids and time pinned the same way), the same repository rows, the
// same frames; every socket must receive the same frames, byte for byte, and the same closes.
// Nothing in the recording is volatile (ids and time are pinned on both sides), so no field is
// masked.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"
#include "meeting_web/server.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "meeting_web/voice_help.h"
#include "net/reactor.h"
#include "pyport/json.h"
#include "testkit/testkit.h"
#include "web_test_support.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;

namespace {

Json load_transcript() {
    const auto path =
        fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" / "meeting_ws.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    return fv::py::loads(text.str());
}

std::string percent_encode(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        }
    }
    return out;
}

std::string query_string(const Json& pairs) {
    std::string query;
    for (const auto& pair : pairs) {
        if (!query.empty()) {
            query += '&';
        }
        query += percent_encode(pair[0].get<std::string>()) + "=" +
                 percent_encode(pair[1].get<std::string>());
    }
    return query;
}

std::string compact(const Json& value) {
    return fv::py::dumps(value);
}

} // namespace

TEST_CASE("the recorded WebSocket transcript replays", "[transcript][meeting_ws]") {
    const Json transcript = load_transcript();
    const Json& context = transcript.at("context");

    fv::FakeClock clock;
    fv::meeting_state::testing::FixedIds ids;
    fv::meeting_state::MeetingState state;
    state.meeting_id = context.at("meeting").at("id").get<std::string>();
    auto store = std::make_shared<fv::meeting_state::MeetingStateStore>(
        std::move(state), nullptr, fv::meeting_state::SegmentHandler{}, clock, ids);
    store->apply("system", std::nullopt, context.at("initial_ops"));
    mw::testing::FakeDashboardEngine engine(store->meeting_id(), store);
    std::vector<Json> segments;
    for (const auto& segment : context.at("segments")) {
        segments.push_back(segment);
    }
    auto repo = std::make_shared<HelloRepository>(context.at("meeting"), std::move(segments));

    fv::net::Reactor reactor;
    mw::MeetingWebServer server(reactor, engine, repo);
    const std::string guest_url = context.at("guest_url").get<std::string>();
    server.hub().set_guest_url_provider([guest_url] { return guest_url; });
    // OpenWhisper's default wake names, as the recording's settings had them.
    server.hub().set_voice_commands(
        [] { return mw::voice_command_guide(Json::object(), fv::legacy_identity()); });
    const auto url = server.start();
    REQUIRE(url.has_value());
    const auto port = static_cast<std::uint16_t>(std::stoi(url->substr(url->rfind(':') + 1)));

    std::map<std::string, std::unique_ptr<Client>> sockets;
    int index = 0;
    int compared = 0;
    for (const auto& step : transcript.at("steps")) {
        ++index;
        INFO("step " << index << ": " << compact(step).substr(0, 300));
        if (step.contains("open")) {
            const auto name = step.at("open").get<std::string>();
            sockets[name] = std::make_unique<Client>(port, query_string(step.at("query")));
        } else if (step.contains("close")) {
            sockets.at(step.at("close").get<std::string>())->close();
        } else {
            auto& socket = *sockets.at(step.at("send").get<std::string>());
            if (step.contains("binary_base64")) {
                const auto bytes = fv::b64decode(step.at("binary_base64").get<std::string>());
                REQUIRE(bytes.has_value());
                socket.send_binary(
                    std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            } else if (step.contains("repeat")) {
                const auto unit = step.at("repeat")[0].get<std::string>();
                const auto count = step.at("repeat")[1].get<std::size_t>();
                std::string text;
                text.reserve(unit.size() * count);
                for (std::size_t i = 0; i < count; ++i) {
                    text += unit;
                }
                socket.send_text(text);
            } else {
                socket.send_text(step.at("text").get<std::string>());
            }
        }
        for (const auto& [name, expected_frames] : step.at("received").items()) {
            auto& socket = *sockets.at(name);
            for (const auto& expected : expected_frames) {
                INFO("socket " << name << ", expecting " << compact(expected).substr(0, 300));
                if (expected.contains("closed")) {
                    CHECK(socket.closed_with() == expected.at("closed").get<std::uint16_t>());
                    ++compared;
                    continue;
                }
                const std::string text = socket.receive_text();
                CHECK(compact(fv::py::loads(text)) == compact(expected.at("message")));
                // The frame's text as ws.py wrote it: json.dumps(message, ensure_ascii=False),
                // which the recorded message re-encodes to exactly.
                fv::py::DumpOptions wire;
                wire.ensure_ascii = false;
                CHECK(text == fv::py::dumps(expected.at("message"), wire));
                ++compared;
            }
        }
    }
    CHECK(index == 42);
    CHECK(compared == 71);
    // Nothing more than OpenWhisper sent.
    for (const auto& [name, socket] : sockets) {
        INFO("socket " << name);
        if (!socket->raw().close_code()) {
            CHECK_FALSE(socket->try_receive(std::chrono::milliseconds(200)).has_value());
        }
    }
    server.stop();
}
