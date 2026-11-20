// llm/sse.h: the WHATWG event-stream rules the providers' streams follow, fed whole and split at
// every byte.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llm/sse.h"

using fv::llm::SseEvent;
using fv::llm::SseParser;

namespace {

struct Seen {
    std::string event, data, id;
    std::optional<int> retry_ms;
    bool operator==(const Seen&) const = default;
};

std::vector<Seen> parse_whole(std::string_view stream, bool finish = true) {
    std::vector<Seen> out;
    SseParser parser;
    const auto keep = [&](const SseEvent& e) {
        out.push_back({e.event, e.data, e.id, e.retry_ms});
    };
    parser.feed(stream, keep);
    if (finish) {
        parser.finish(keep);
    }
    return out;
}

// The same stream fed one byte per call.
std::vector<Seen> parse_bytes(std::string_view stream, bool finish = true) {
    std::vector<Seen> out;
    SseParser parser;
    const auto keep = [&](const SseEvent& e) {
        out.push_back({e.event, e.data, e.id, e.retry_ms});
    };
    for (const char c : stream) {
        parser.feed(std::string_view(&c, 1), keep);
    }
    if (finish) {
        parser.finish(keep);
    }
    return out;
}

// Every split of the stream into two calls gives what one call gives.
void check_every_split(std::string_view stream, const std::vector<Seen>& expected) {
    CHECK(parse_whole(stream) == expected);
    CHECK(parse_bytes(stream) == expected);
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        std::vector<Seen> out;
        SseParser parser;
        const auto keep = [&](const SseEvent& e) {
            out.push_back({e.event, e.data, e.id, e.retry_ms});
        };
        parser.feed(stream.substr(0, cut), keep);
        parser.feed(stream.substr(cut), keep);
        parser.finish(keep);
        INFO("cut at " << cut);
        CHECK(out == expected);
    }
}

} // namespace

TEST_CASE("sse: LF, CRLF and CR all end lines, split anywhere", "[sse]") {
    const std::vector<Seen> expected{{"message", "one", "", std::nullopt},
                                     {"message", "two", "", std::nullopt},
                                     {"message", "three", "", std::nullopt}};
    check_every_split("data: one\n\ndata: two\r\n\r\ndata: three\r\r", expected);
}

TEST_CASE("sse: a CRLF split between two calls is one line end", "[sse]") {
    std::vector<Seen> out;
    SseParser parser;
    const auto keep = [&](const SseEvent& e) {
        out.push_back({e.event, e.data, e.id, e.retry_ms});
    };
    parser.feed("data: a\r", keep);
    parser.feed("\n", keep);
    CHECK(out.empty());
    parser.feed("\r", keep);
    REQUIRE(out.size() == 1);
    parser.feed("\ndata: b\r\n\r\n", keep);
    REQUIRE(out.size() == 2);
    CHECK(out[1].data == "b");
}

TEST_CASE("sse: a leading BOM is dropped once, also when split", "[sse]") {
    const std::vector<Seen> expected{{"message", "x", "", std::nullopt}};
    check_every_split("\xEF\xBB\xBF"
                      "data: x\n\n",
                      expected);
    // A second BOM is part of the line: here a field named "\xEF\xBB\xBF" "data", ignored.
    CHECK(parse_whole("\xEF\xBB\xBF\xEF\xBB\xBF"
                      "data: x\n\n")
              .empty());
    // Bytes that only start like the BOM are kept.
    const auto kept = parse_whole("data: \xEF\xBB"
                                  "z\n\n");
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].data == "\xEF\xBB"
                          "z");
}

TEST_CASE("sse: comments and keep-alives make no event", "[sse]") {
    const std::vector<Seen> expected{{"message", "{\"a\":1}", "", std::nullopt}};
    check_every_split(": OPENROUTER PROCESSING\n\n: OPENROUTER PROCESSING\n\ndata: {\"a\":1}\n\n",
                      expected);
    CHECK(parse_whole(":\n\n:comment\n\n").empty());
}

TEST_CASE("sse: data lines join with LF; one space after the colon is dropped", "[sse]") {
    check_every_split("data: first\ndata:second\ndata:  third\ndata\n\n",
                      {{"message", "first\nsecond\n third\n", "", std::nullopt}});
    // A block without data dispatches nothing and forgets its event type.
    check_every_split("event: ping\n\ndata: x\n\n", {{"message", "x", "", std::nullopt}});
    // "data" with an empty value is an event with empty data.
    check_every_split("data\n\n", {{"message", "", "", std::nullopt}});
}

TEST_CASE("sse: event names, ids that persist, retry and unknown fields", "[sse]") {
    check_every_split("event: message_start\ndata: {}\n\n"
                      "id: 7\nretry: 1500\ndata: a\n\n"
                      "data: b\n\n"
                      "id\ndata: c\n\n"
                      "retry: 12x\nfoo: bar\nevent\ndata: d\n\n",
                      {{"message_start", "{}", "", std::nullopt},
                       {"message", "a", "7", 1500},
                       {"message", "b", "7", std::nullopt},
                       {"message", "c", "", std::nullopt},
                       {"message", "d", "", std::nullopt}});
    // An id holding NUL is ignored.
    static constexpr char kWithNul[] = "id: 1\n\nid: a\0b\ndata: z\n\n";
    const auto nul = parse_whole(std::string_view(kWithNul, sizeof kWithNul - 1));
    REQUIRE(nul.size() == 1);
    CHECK(nul[0].id == "1");
}

TEST_CASE("sse: [DONE] is ordinary data", "[sse]") {
    check_every_split(
        "data: {\"x\":1}\n\ndata: [DONE]\n\n",
        {{"message", "{\"x\":1}", "", std::nullopt}, {"message", "[DONE]", "", std::nullopt}});
}

TEST_CASE("sse: finish dispatches an event whose blank line never came", "[sse]") {
    // Without finish the last event waits for its blank line.
    CHECK(parse_whole("data: a\n\ndata: b", false).size() == 1);
    CHECK(parse_whole("data: a\n\ndata: b\n", false).size() == 1);
    check_every_split("data: a\n\ndata: b",
                      {{"message", "a", "", std::nullopt}, {"message", "b", "", std::nullopt}});
    check_every_split("data: a\n\ndata: b\r\n",
                      {{"message", "a", "", std::nullopt}, {"message", "b", "", std::nullopt}});
    // finish leaves a fresh parser behind.
    std::vector<Seen> out;
    SseParser parser;
    const auto keep = [&](const SseEvent& e) {
        out.push_back({e.event, e.data, e.id, e.retry_ms});
    };
    parser.feed("id: 3\nevent: x\ndata: a", keep);
    parser.finish(keep);
    parser.feed("\xEF\xBB\xBF"
                "data: b\n\n",
                keep);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == Seen{"x", "a", "3", std::nullopt});
    CHECK(out[1] == Seen{"message", "b", "", std::nullopt});
}
