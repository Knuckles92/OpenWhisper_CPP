// Ported from tests/test_meeting_diarize.py::TestSharedAssignHelpers, plus the golden vectors of
// meeting.diarize.assign.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "diarize/assign.h"
#include "diarize_support.h"
#include "golden/golden.h"

using fv::diarize::assign_from_frames;
using fv::diarize::Diarizer;
using fv::diarize::refresh_labels;
using fv::diarize::Segment;
using fv::diarize::test::lcg_int16;
using fv::diarize::test::segment;

namespace {

// The test's Diarizer: labels everything "p_first" and reports whatever its map says now.
class FirstLabelDiarizer final : public Diarizer {
public:
    std::optional<std::string> assign(Segment& s, std::span<const float>, int) override {
        labels[s.id] = "p_first";
        return "p_first";
    }
    void pin(const std::string&, const std::string&) override {}
    std::optional<std::string> current_label(const std::string& id) const override {
        const auto found = labels.find(id);
        return found == labels.end() ? std::nullopt : std::optional<std::string>(found->second);
    }
    bool is_available() const override { return true; }

    std::map<std::string, std::string> labels;
};

// The golden case file's stub: records each call, answers "p_<id>", no label for "_none",
// throws for "_boom".
class RecordingDiarizer final : public Diarizer {
public:
    struct Call {
        std::string id;
        std::vector<float> audio;
        int rate;
    };

    std::optional<std::string> assign(Segment& s, std::span<const float> audio, int rate) override {
        calls.push_back({s.id, {audio.begin(), audio.end()}, rate});
        if (s.id.ends_with("_boom")) {
            throw std::runtime_error("diarizer failed");
        }
        if (s.id.ends_with("_none")) {
            return std::nullopt;
        }
        return "p_" + s.id;
    }
    void pin(const std::string&, const std::string&) override {}
    bool is_available() const override { return true; }

    std::vector<Call> calls;
};

} // namespace

// from tests/test_meeting_diarize.py::TestSharedAssignHelpers
TEST_CASE("TestSharedAssignHelpers::test_refresh_labels_applies_later_recluster_corrections",
          "[test_meeting_diarize]") {
    FirstLabelDiarizer diarizer;
    std::vector<Segment> segments{segment("sg_1", 0.0), segment("sg_2", 1.0)};
    const std::vector<std::int16_t> frames(32000, 0);
    const auto labeled = assign_from_frames(diarizer, segments, frames, 16000, 0.0);
    CHECK(labeled == std::vector<std::string>{"sg_1", "sg_2"});
    for (const Segment& s : segments) {
        CHECK(s.speaker_participant_id == "p_first");
        CHECK(s.speaker_source == "diarizer");
    }

    diarizer.labels["sg_1"] = "p_moved";
    refresh_labels(diarizer, segments);

    CHECK(segments[0].speaker_participant_id == "p_moved");
    CHECK(segments[1].speaker_participant_id == "p_first");
}

TEST_CASE("refresh_labels keeps a label the diarizer does not know", "[assign]") {
    FirstLabelDiarizer diarizer;
    std::vector<Segment> segments{segment("sg_1", 0.0)};
    segments[0].speaker_participant_id = "p_kept";
    refresh_labels(diarizer, segments);
    CHECK(segments[0].speaker_participant_id == "p_kept");
}

TEST_CASE("assign_from_frames replays the golden vectors", "[golden][assign]") {
    fv::golden::for_each(
        "meeting.diarize.assign", "assign_from_frames", [](const fv::golden::Case& c) {
            std::vector<Segment> segments;
            for (const auto& spec : c.args()[0]) {
                segments.push_back(Segment{spec[0].get<std::string>(),
                                           fv::golden::decode<double>(spec[1]),
                                           fv::golden::decode<double>(spec[2]),
                                           {},
                                           std::nullopt,
                                           "channel"});
            }
            const auto frames = lcg_int16(c.arg<std::size_t>(1), c.arg<std::uint64_t>(2));
            RecordingDiarizer diarizer;
            int unlabeled = 0;
            const auto labeled =
                assign_from_frames(diarizer, segments, frames, c.arg<int>(3), c.arg<double>(4),
                                   [&](const Segment&) { ++unlabeled; });

            const fv::Json& expected = c.raw().at("result");
            CHECK(labeled == fv::golden::decode<std::vector<std::string>>(expected.at("labeled")));
            CHECK(unlabeled == expected.at("unlabeled").get<int>());
            const fv::Json& calls = expected.at("calls");
            REQUIRE(diarizer.calls.size() == calls.size());
            for (std::size_t i = 0; i < calls.size(); ++i) {
                const auto& call = diarizer.calls[i];
                INFO("call " << i << " for " << call.id);
                CHECK(call.id == calls[i][0].get<std::string>());
                REQUIRE(call.audio.size() == calls[i][1].get<std::size_t>());
                CHECK(call.rate == calls[i][2].get<int>());
                const auto head = fv::golden::decode<std::vector<double>>(calls[i][3]);
                const auto tail = fv::golden::decode<std::vector<double>>(calls[i][4]);
                for (std::size_t k = 0; k < head.size(); ++k) {
                    CHECK(std::abs(call.audio[k] - head[k]) <= c.float_tolerance());
                }
                for (std::size_t k = 0; k < tail.size(); ++k) {
                    const float actual = call.audio[call.audio.size() - tail.size() + k];
                    CHECK(std::abs(actual - tail[k]) <= c.float_tolerance());
                }
            }
            const fv::Json& speakers = expected.at("speakers");
            REQUIRE(speakers.size() == segments.size());
            for (std::size_t i = 0; i < segments.size(); ++i) {
                CHECK(segments[i].id == speakers[i][0].get<std::string>());
                CHECK(segments[i].speaker_participant_id ==
                      fv::golden::decode<std::optional<std::string>>(speakers[i][1]));
                CHECK(segments[i].speaker_source == speakers[i][2].get<std::string>());
            }
        });
}
