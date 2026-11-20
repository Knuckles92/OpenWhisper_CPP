#pragma once

// Shared pieces of the streaming tests: the hand-written streams of tests/fixtures/llm/streams/,
// the profiles they run against, and events as the fixtures spell them.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "generation_test_support.h"
#include "llm/profiles.h"
#include "llm/streaming.h"
#include "pyport/json.h"
#include "sse_provider.h"
#include "testkit/testkit.h"

namespace fv::llm::test {

struct StreamFixture {
    std::string name;
    Json document;
};

inline const std::vector<StreamFixture>& stream_fixtures() {
    static const std::vector<StreamFixture> fixtures = [] {
        std::vector<StreamFixture> out;
        const auto folder = testkit::source_root() / "tests" / "fixtures" / "llm" / "streams";
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() != ".json") {
                continue;
            }
            std::ifstream file(entry.path(), std::ios::binary);
            out.push_back({entry.path().stem().string(),
                           py::loads(std::string((std::istreambuf_iterator<char>(file)),
                                                 std::istreambuf_iterator<char>()))});
        }
        std::ranges::sort(out, {}, &StreamFixture::name);
        return out;
    }();
    return fixtures;
}

// A fixture's frames as the provider writes them: a JSON frame as one SSE event (with its
// "event:" line when the fixture is `named`), a string as raw bytes.
inline std::vector<std::string> fixture_frames(const Json& fixture) {
    const bool named = fixture.value("named", false);
    std::vector<std::string> out;
    for (const auto& frame : fixture.at("frames")) {
        out.push_back(frame.is_string() ? frame.get<std::string>()
                                        : testing::SseReply::frame(frame, named));
    }
    return out;
}

inline std::string fixture_body(const Json& fixture) {
    std::string out;
    for (const auto& frame : fixture_frames(fixture)) {
        out += frame;
    }
    return out;
}

// The non-streamed reply the stream amounts to.
inline Json fixture_equivalent(const Json& fixture) {
    if (fixture.contains("equivalent_reply")) {
        return fixture.at("equivalent_reply");
    }
    return reply_body(fixture.at("equivalent").get<std::string>());
}

// A custom endpoint whose saved metadata names `protocol` for the model "synthetic".
inline TextLLMProfile synthetic_profile(const std::string& protocol, const std::string& base_url,
                                        Json extra = Json::object()) {
    TextLLMProfile profile;
    profile.id = "synthetic-endpoint";
    profile.name = "Synthetic";
    profile.kind = "custom";
    profile.base_url = base_url;
    Json metadata{{"model", "synthetic"}, {"protocol", protocol}};
    for (const auto& [key, value] : extra.items()) {
        metadata[key] = value;
    }
    profile.model_metadata = std::move(metadata);
    return profile;
}

// [name, text, tool_call_id, tool_name] with the trailing empty strings dropped, as the fixtures
// write events.
inline Json event_json(const StreamEvent& event) {
    Json out = Json::array({std::string(stream_delta_name(event.kind)), event.text,
                            event.tool_call_id, event.tool_name});
    while (out.size() > 1 && out.back() == "") {
        out.erase(out.size() - 1);
    }
    return out;
}

inline Json events_json(const std::vector<StreamEvent>& events) {
    Json out = Json::array();
    for (const auto& event : events) {
        out.push_back(event_json(event));
    }
    return out;
}

inline Json default_messages() {
    return Json::array({Json{{"role", "system"}, {"content", "Use evidence."}},
                        Json{{"role", "user"}, {"content", "Budget?"}}});
}

} // namespace fv::llm::test
