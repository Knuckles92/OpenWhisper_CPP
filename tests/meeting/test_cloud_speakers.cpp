#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/upload_encoder.h"
#include "audio/wav_writer.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "meeting/cloud_pass.h"
#include "meeting/openai_diarize.h"
#include "meeting_capture/session_audio.h"
#include "runtime_support.h"
#include "settings/keys.h"

namespace {

using fv::Json;
using namespace fv::meeting;
using fv::meeting::runtime_test::RuntimeRig;

Json segment(std::string id, double start, double end, bool pinned = false) {
    return {{"id", std::move(id)}, {"start_s", start}, {"end_s", end}, {"speaker_pinned", pinned}};
}

Json turn(std::string speaker, double start, double end) {
    return {{"speaker", std::move(speaker)}, {"start", start}, {"end", end}};
}

TEST_CASE("speaker windows overlap and short references expand within the file",
          "[speaker_upload]") {
    using namespace cloud_audio;
    CHECK(plan_windows(0, 16000).empty());
    CHECK((plan_windows(16000 * 10, 16000) == std::vector<Range>{{0, 16000 * 10}}));
    const auto longer = plan_windows(16000 * 96, 16000, 85'000);
    REQUIRE(longer.size() > 2);
    CHECK(longer.front().first == 0);
    CHECK(longer.back().second == 16000 * 96);
    CHECK(longer[1].first == longer[0].second - 32000);
    CHECK((clip_sample_range(16000 * 100, 16000, 5.0, 5.1) == Range{64800, 96800}));
    CHECK((clip_sample_range(16000 * 100, 16000, 35, 70) == Range{560000, 720000}));
    CHECK((clip_sample_range(16000 * 3, 16000, -1, 0.01) == Range{0, 32000}));
}

TEST_CASE("OpenAI turns label only the greatest-overlap unpinned speech", "[speaker_upload]") {
    const std::vector<Json> rows{segment("cross", 2, 4), segment("best", 2, 5),
                                 segment("pinned", 0, 1, true), segment("gap", 13, 14),
                                 segment("later", 6, 8)};
    const std::vector<Json> unordered{turn("b", 3, 5), turn("a", 0, 3), turn("c", 7, 12),
                                      turn("", 0, 49), turn("wrong", 5, 5)};
    const auto mapped = map_turns_to_segments(rows, unordered);
    CHECK((mapped == NamedSpeakers{{"cross", "a"}, {"best", "b"}, {"later", "c"}}));
    CHECK(overlap_duration(0, 1, 1, 2) == 0);
    const auto relative = project_window_turns(Json::array({turn("z", 7, 13)}), 10, 10);
    REQUIRE(relative.size() == 1);
    CHECK(relative.front().at("start") == 17.0);
    CHECK(relative.front().at("end") == 20.0);
}

TEST_CASE("saved speaker reference votes prefer pinned named matches without duplicates",
          "[speaker_upload]") {
    auto first = segment("first", 0, 2);
    first["speaker_participant_id"] = "person";
    auto second = segment("second", 2, 4);
    second["speaker_participant_id"] = "person";
    const auto resolved = resolve_speaker_participants(
        {{"weak", {"first"}}, {"majority", {"first", "second"}}, {"new", {"unknown"}}},
        {{"first", first}, {"second", second}});
    const SpeakerMap expected{
        {"new", std::nullopt}, {"majority", std::string("person")}, {"weak", std::nullopt}};
    CHECK(resolved == expected);
    const auto override = resolve_speaker_participants({{"api", {"first", "second"}}},
                                                       {{"first", first}, {"second", second}},
                                                       {{"api", "named-person"}});
    REQUIRE(override.size() == 1);
    CHECK(override.front().second == "named-person");
    CHECK(speaker_slug("  Alex F. / ") == "Alex_F");
}

TEST_CASE("the speaker transport posts only audio fields to the fixed OpenAI endpoint",
          "[speaker_upload]") {
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    http->push({.body = R"({"segments":[{"speaker":"voice_1","start":1,"end":2}]})"});
    const auto diarize = make_openai_diarize(http, "fake-secret");
    const auto result = diarize({"fLaC-test",
                                 "system-audio.flac",
                                 "audio/flac",
                                 {"voice_1"},
                                 {"data:audio/flac;base64,AAAA"}},
                                {});
    REQUIRE(result.size() == 1);
    CHECK(result[0].at("speaker") == "voice_1");
    const auto calls = http->requests();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().url == "https://api.openai.com/v1/audio/transcriptions");
    CHECK_FALSE(calls.front().follow_redirects);
    CHECK(calls.front().bypass_proxy);
    const auto parts = http->multipart_parts().front();
    REQUIRE(parts.size() == 6);
    CHECK(parts[0].name == "model");
    CHECK(std::get<std::string>(parts[0].data) == "gpt-4o-transcribe-diarize");
    CHECK(parts[3].name == "file");
    CHECK(parts[3].content_type == "audio/flac");
    CHECK(std::get<std::string>(parts[3].data) == "fLaC-test");
    CHECK(parts[4].name == "known_speaker_names[]");
    CHECK(parts[5].name == "known_speaker_references[]");
}

struct UploadFixture {
    int calls = 0;
    std::vector<std::int16_t> system_audio = std::vector<std::int16_t>(48000, 31);
    RuntimeRig rig{[this](MeetingRuntimeDeps& deps) {
        deps.approved_speaker_decoder = [this] {
            return [this](const DiarizeRequest& request, const fv::CancelToken&) -> Json {
                ++calls;
                CHECK(request.audio == fv::audio::encode_upload_flac(system_audio, 16000));
                CHECK(request.filename == "system-audio.flac");
                return Json::array({turn("a", 0.0, 2.95)});
            };
        };
    }};
    std::string id = "m_test_upload";
    std::filesystem::path spool =
        rig.root.path() / "meetings" / fv::path_from_utf8(id.substr(0, 12));

    UploadFixture() {
        std::filesystem::create_directories(spool);
        write_wav("loopback", system_audio);
        write_wav("mic", std::vector<std::int16_t>(48000, 190));
        rig.make_ended_meeting(
            id, false, "completed", "2026-11-17T09:00:00",
            {{"title", "Planning review"}, {"spool_dir", fv::path_to_utf8(spool)}});
        add_row("sg_loopback", "loopback", 0, 1, false);
        add_row("sg_pinned", "loopback", 1, 2, true);
        add_row("sg_mic", "mic", 0, 2, false);
    }

    void write_wav(std::string_view channel, const std::vector<std::int16_t>& samples) {
        fv::audio::WavWriter file(session_wav_path(spool, channel), {.rate = 16000});
        file.write(samples);
        file.commit(std::nullopt);
    }

    void add_row(std::string row_id, std::string channel, double start, double end, bool pinned) {
        fv::meeting_state::TranscriptSegment row;
        row.meeting_id = id;
        row.segment_id = std::move(row_id);
        row.channel = std::move(channel);
        row.start_s = start;
        row.end_s = end;
        row.speaker_pinned = pinned;
        row.text = "Keep these words.";
        rig.repo->add_segments({row});
    }

    void request(std::function<bool(const SpeakerUploadOffer&)> confirm) {
        fv::meeting::runtime_test::on_ui(
            rig.ui, [&] { rig.rt->request_speaker_upload(id, std::move(confirm)); });
        rig.drain();
    }
};

TEST_CASE("each saved speaker pass asks again and sends only system audio after approval",
          "[speaker_upload]") {
    REQUIRE(fv::audio::has_upload_flac());
    UploadFixture fixture;
    fixture.rig.settings.save_setting(fv::keys::meeting_audio_upload_consent_given.key(), true);
    fixture.rig.settings.save_setting(fv::keys::meeting_speaker_id_backend.key(), "openai");
    unsigned prompts = 0;
    fixture.request([&](const SpeakerUploadOffer& offer) {
        ++prompts;
        CHECK(offer.title == "Planning review");
        CHECK(offer.meeting_id == fixture.id);
        return false;
    });
    CHECK(prompts == 1);
    CHECK(fixture.calls == 0);
    fixture.request([&](const SpeakerUploadOffer&) {
        ++prompts;
        return true;
    });
    CHECK(fixture.calls == 1);
    const auto changed = fixture.rig.repo->get_segment(fixture.id, "sg_loopback");
    REQUIRE(changed);
    CHECK(changed->at("speaker_participant_id").is_string());
    CHECK(changed->at("text") == "Keep these words.");
    CHECK(fixture.rig.repo->get_segment(fixture.id, "sg_pinned")
              ->at("speaker_participant_id")
              .is_null());
    CHECK(fixture.rig.repo->get_segment(fixture.id, "sg_mic")
              ->at("speaker_participant_id")
              .is_null());
    fixture.request([&](const SpeakerUploadOffer&) {
        ++prompts;
        return false;
    });
    CHECK(prompts == 3);
    CHECK(fixture.calls == 1);
}

TEST_CASE("a changed or out-of-folder recording never uploads from an old approval",
          "[speaker_upload]") {
    UploadFixture fixture;
    fixture.request([&](const SpeakerUploadOffer&) {
        fixture.rig.repo->update_meeting(fixture.id, Json{{"title", "Changed"}});
        return true;
    });
    CHECK(fixture.calls == 0);
    fixture.request([&](const SpeakerUploadOffer&) {
        fixture.write_wav("loopback", std::vector<std::int16_t>(32000, 12));
        return true;
    });
    CHECK(fixture.calls == 0);
    const auto previous = fixture.rig.root.path();
    fixture.rig.repo->update_meeting(fixture.id, Json{{"spool_dir", fv::path_to_utf8(previous)}});
    bool asked = false;
    fixture.request([&](const SpeakerUploadOffer&) {
        asked = true;
        return true;
    });
    CHECK_FALSE(asked);
    CHECK(fixture.calls == 0);
}

TEST_CASE("stale approval or cancellation stops later speaker windows without relabeling",
          "[speaker_upload]") {
    UploadFixture fixture;
    fixture.write_wav("loopback", std::vector<std::int16_t>(16'000 * 20, 30));
    const auto root = fixture.rig.root.path() / "meetings";
    const auto offer = describe_speaker_upload(*fixture.rig.repo, fixture.id, root);
    RerunRequest request;
    fv::CancelSource stop;
    request.repository = fixture.rig.repo;
    request.clock = &fixture.rig.clock;
    request.cancel = stop.token();
    bool change = false;
    SECTION("the approved audio file is replaced") {
        change = true;
    }
    SECTION("the run is canceled") {}
    int calls = 0;
    CloudSpeakerOptions options;
    options.byte_budget = 160'000;
    options.encode = [](std::span<const std::int16_t>, int) {
        return "synthetic";
    };
    options.diarize = [&](const DiarizeRequest&, const fv::CancelToken&) {
        ++calls;
        if (change) {
            fixture.write_wav("loopback", std::vector<std::int16_t>(48000, 10));
        } else {
            stop.cancel();
        }
        return Json::array({turn("voice", 0, 2)});
    };
    const auto result = rerun_approved_speakers(offer, root, request, options);
    CHECK(result.at("ok") == false);
    CHECK(result.at("windows").get<int>() > 1);
    CHECK(calls == 1);
    CHECK(fixture.rig.repo->get_segment(fixture.id, "sg_loopback")
              ->at("speaker_participant_id")
              .is_null());
}

TEST_CASE("the saved OpenAI speaker action expires on the announced shutdown date",
          "[speaker_upload]") {
    using namespace std::chrono;
    UploadFixture fixture;
    fixture.rig.clock.set_zone("UTC");
    fixture.rig.clock.set_utc(sys_days{year{2027} / 2 / 26} + hours{12});
    CHECK_FALSE(fixture.rig.rt->can_offer_speaker_upload());
    bool asked = false;
    fixture.request([&](const SpeakerUploadOffer&) {
        asked = true;
        return true;
    });
    CHECK_FALSE(asked);
    CHECK(fixture.calls == 0);
}

} // namespace

// from tests/test_meeting_cloud_speakers.py::TestOverlapMapping::test_zero_overlap_helper,
// TestParticipantResolution::test_majority_vote_reuses_local_label,
// TestParticipantResolution::test_unlabeled_segments_create_new and
// TestParticipantResolution::test_speaker_slug_is_short
TEST_CASE("cloud speaker helpers: overlap, majority vote, unlabeled rows and slugs",
          "[speaker_upload]") {
    CHECK(overlap_duration(0.0, 1.0, 1.0, 2.0) == 0.0);
    CHECK(overlap_duration(0.0, 2.0, 1.0, 3.0) == 1.0);
    auto labeled = [](std::string id, double start, std::string participant) {
        auto row = segment(std::move(id), start, start + 1);
        row["speaker_participant_id"] = std::move(participant);
        return row;
    };
    const auto majority = resolve_speaker_participants({{"speaker_0", {"sg_a", "sg_b", "sg_c"}}},
                                                       {{"sg_a", labeled("sg_a", 0, "p_1")},
                                                        {"sg_b", labeled("sg_b", 1, "p_1")},
                                                        {"sg_c", labeled("sg_c", 2, "p_2")}});
    CHECK(majority == SpeakerMap{{"speaker_0", std::string("p_1")}});
    const auto unlabeled =
        resolve_speaker_participants({{"speaker_0", {"sg_a"}}}, {{"sg_a", segment("sg_a", 0, 1)}});
    CHECK(unlabeled == SpeakerMap{{"speaker_0", std::nullopt}});
    CHECK(speaker_slug("Sarah Chen") == "Sarah_Chen");
    CHECK(speaker_slug("!!!") == "speaker");
}

// from tests/test_meeting_cloud_speakers.py::test_cloud_decoder_internal_typeerror_is_not_retried
TEST_CASE("a decoder that fails inside is asked once and the pass fails", "[speaker_upload]") {
    UploadFixture fixture;
    const auto root = fixture.rig.root.path() / "meetings";
    const auto offer = describe_speaker_upload(*fixture.rig.repo, fixture.id, root);
    RerunRequest request;
    request.repository = fixture.rig.repo;
    request.clock = &fixture.rig.clock;
    int calls = 0;
    CloudSpeakerOptions options;
    options.encode = [](std::span<const std::int16_t>, int) {
        return "audio";
    };
    options.diarize = [&](const DiarizeRequest&, const fv::CancelToken&) -> Json {
        ++calls;
        throw std::invalid_argument("decoder internal failure");
    };
    const auto result = rerun_approved_speakers(offer, root, request, options);
    CHECK(result.at("ok") == false);
    // OpenWhisper_CPP reports its own sentence rather than the decoder's text (a provider body can
    // quote what was sent).
    CHECK(result.at("error") == "OpenAI speaker identification could not finish.");
    CHECK(calls == 1);
    CHECK(fixture.rig.repo->get_segment(fixture.id, "sg_loopback")
              ->at("speaker_participant_id")
              .is_null());
}
