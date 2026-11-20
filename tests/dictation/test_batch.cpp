// The multi-file upload request model and its text helpers (services/batch_upload.py).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dictation/batch.h"
#include "golden/golden.h"
#include "pyport/text.h"

namespace {

using fv::dictation::batch_source_name;
using fv::dictation::BatchItem;
using fv::dictation::BatchItemResult;
using fv::dictation::BatchRelation;
using fv::dictation::BatchResult;
using fv::dictation::BatchUploadRequest;
using fv::dictation::compose_batch_cleanup_prompt;
using fv::dictation::format_batch_transcript;
using fv::dictation::join_raw_parts;

constexpr const char* kModule = "services.batch_upload";

BatchUploadRequest request(BatchRelation relation, int count = 2,
                           std::string custom_instructions = "", bool custom_combine = false) {
    BatchUploadRequest result;
    for (int i = 0; i < count; ++i) {
        result.items.push_back({std::format("/audio/part{}.mp3", i), 60.0});
    }
    result.relation = relation;
    result.custom_instructions = std::move(custom_instructions);
    result.custom_combine = custom_combine;
    return result;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Python's str.format of a config template, for the tests that compare with it.
std::string format_note(std::string_view text, std::size_t count, std::string_view names) {
    std::string out(text);
    const auto replace = [&out](std::string_view field, std::string_view value) {
        const std::size_t at = out.find(field);
        if (at != std::string::npos) {
            out.replace(at, field.size(), value);
        }
    };
    replace("{count}", std::to_string(count));
    replace("{names}", names);
    return out;
}

// The case files' request: {"paths", "relation", "custom_instructions", "custom_combine"}.
BatchUploadRequest request_from(const fv::Json& spec) {
    BatchUploadRequest result;
    for (const auto& path : spec.at("paths")) {
        result.items.push_back({path.get<std::string>(), std::nullopt});
    }
    result.relation = *fv::dictation::batch_relation_from(spec.at("relation").get<std::string>());
    result.custom_instructions = spec.value("custom_instructions", std::string());
    result.custom_combine = spec.value("custom_combine", false);
    return result;
}

} // namespace

TEST_CASE("sequential combines and separate does not", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestBatchUploadRequest::test_sequential_combines_and_separate_does_not
    CHECK(request(BatchRelation::Sequential).combine());
    CHECK_FALSE(request(BatchRelation::Separate).combine());
}

TEST_CASE("custom follows its own checkbox", "[test_batch_upload]") {
    // from tests/test_batch_upload.py::TestBatchUploadRequest::test_custom_follows_its_own_checkbox
    CHECK(request(BatchRelation::Custom, 2, "", true).combine());
    CHECK_FALSE(request(BatchRelation::Custom, 2, "", false).combine());
}

TEST_CASE("separate has no batch context", "[test_batch_upload]") {
    // from tests/test_batch_upload.py::TestBatchUploadRequest::test_separate_has_no_batch_context
    // Separate must clean exactly like a single-file upload.
    const auto req = request(BatchRelation::Separate);
    CHECK_FALSE(req.batch_context().has_value());
    CHECK_FALSE(req.batch_context(&req.items[0]).has_value());
}

TEST_CASE("sequential context names the parts in order", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestBatchUploadRequest::test_sequential_context_names_the_parts_in_order
    const auto context = request(BatchRelation::Sequential, 3).batch_context();
    REQUIRE(context.has_value());
    CHECK(contains(*context, "3 consecutive parts"));
    CHECK(contains(*context, "part0.mp3, part1.mp3, part2.mp3"));
}

TEST_CASE("custom combined context carries the user text", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestBatchUploadRequest::test_custom_combined_context_carries_the_user_text
    const auto req =
        request(BatchRelation::Custom, 2, "  Two interviews with the same client.  ", true);
    const auto context = req.batch_context();
    REQUIRE(context.has_value());
    CHECK(context->starts_with(format_note(fv::dictation::TRANSCRIPT_BATCH_CUSTOM_COMBINED_NOTE, 2,
                                           "part0.mp3, part1.mp3")));
    CHECK(context->ends_with("Two interviews with the same client."));
}

TEST_CASE("custom per file context names the current file", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestBatchUploadRequest::test_custom_per_file_context_names_the_current_file
    const auto req = request(BatchRelation::Custom, 3, "Voice memos.");
    const auto context = req.batch_context(&req.items[1]);
    REQUIRE(context.has_value());
    CHECK(contains(*context, "This transcript is part1.mp3, one of 3 recordings"));
    CHECK(context->ends_with("Voice memos."));
}

TEST_CASE("custom with nothing typed has no context", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestBatchUploadRequest::test_custom_with_nothing_typed_has_no_context
    const auto req = request(BatchRelation::Custom, 2, "   ");
    CHECK_FALSE(req.batch_context().has_value());
    CHECK_FALSE(req.batch_context(&req.items[0]).has_value());
}

TEST_CASE("base prompt comes first then header context and guard", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestComposeBatchCleanupPrompt::test_base_prompt_comes_first_then_header_context_and_guard
    const std::string prompt = compose_batch_cleanup_prompt("BASE\n\nRules: 1. x", "CONTEXT");
    CHECK(prompt.starts_with("BASE\n\nRules: 1. x\n\n"));
    const std::size_t header_at = prompt.find(fv::dictation::TRANSCRIPT_BATCH_CONTEXT_HEADER);
    const std::size_t context_at = prompt.find("CONTEXT");
    const std::size_t guard_at = prompt.find(fv::dictation::TRANSCRIPT_BATCH_CONTEXT_GUARD);
    REQUIRE(header_at != std::string::npos);
    REQUIRE(guard_at != std::string::npos);
    CHECK(header_at < context_at);
    CHECK(context_at < guard_at);
}

TEST_CASE("join raw parts uses blank lines and drops empties", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestTextHelpers::test_join_raw_parts_uses_blank_lines_and_drops_empties
    const std::vector<std::string> parts{"  one ", "", "   ", "two"};
    CHECK(join_raw_parts(parts) == "one\n\ntwo");
}

TEST_CASE("join raw parts of nothing is empty", "[test_batch_upload]") {
    // from tests/test_batch_upload.py::TestTextHelpers::test_join_raw_parts_of_nothing_is_empty
    CHECK(join_raw_parts({}).empty());
    const std::vector<std::string> blanks{"", "  "};
    CHECK(join_raw_parts(blanks).empty());
}

TEST_CASE("format batch transcript heads each section with its name", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestTextHelpers::test_format_batch_transcript_heads_each_section_with_its_name
    const std::vector<std::pair<std::string, std::string>> sections{{"a.wav", " Hello. "},
                                                                    {"b.wav", "Bye."}};
    CHECK(format_batch_transcript(sections) == "## a.wav\n\nHello.\n\n## b.wav\n\nBye.");
}

TEST_CASE("batch source name lists every file when short", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestTextHelpers::test_batch_source_name_lists_every_file_when_short
    const std::vector<std::string> names{"a.mp3", "b.mp3"};
    CHECK(batch_source_name(names) == "2 files: a.mp3, b.mp3");
}

TEST_CASE("batch source name drops trailing names past the limit", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestTextHelpers::test_batch_source_name_drops_trailing_names_past_the_limit
    std::vector<std::string> names;
    for (int i = 0; i < 20; ++i) {
        names.push_back(std::format("recording_{:02}.mp3", i));
    }
    const std::string label = batch_source_name(names, 60);
    CHECK(label.starts_with("20 files: recording_00.mp3"));
    CHECK(label.ends_with(" more"));
    CHECK(label.size() <= 60);
    CHECK_FALSE(contains(label, "recording_19.mp3"));
}

TEST_CASE("batch source name always keeps the first file", "[test_batch_upload]") {
    // from
    // tests/test_batch_upload.py::TestTextHelpers::test_batch_source_name_always_keeps_the_first_file
    const std::vector<std::string> names{"a_very_long_recording_name.mp3", "b.mp3"};
    CHECK(batch_source_name(names, 10) == "2 files: a_very_long_recording_name.mp3, +1 more");
}

TEST_CASE("relations read and write the stored values") {
    for (const auto relation :
         {BatchRelation::Separate, BatchRelation::Sequential, BatchRelation::Custom}) {
        CHECK(fv::dictation::batch_relation_from(fv::dictation::to_string(relation)) == relation);
    }
    CHECK_FALSE(fv::dictation::batch_relation_from("Separate").has_value());
    CHECK_FALSE(fv::dictation::batch_relation_from("").has_value());
}

TEST_CASE("an item result succeeded unless it has an error") {
    BatchItemResult result;
    CHECK(result.succeeded());
    result.error = "";
    CHECK_FALSE(result.succeeded());
}

TEST_CASE("golden batch_upload combine and source names", "[golden]") {
    fv::golden::for_each(kModule, "combine", [](const fv::golden::Case& c) {
        CHECK(request_from(c.args()[0]).combine() == c.result<bool>());
    });
    fv::golden::for_each(kModule, "source_names", [](const fv::golden::Case& c) {
        CHECK(request_from(c.args()[0]).source_names() == c.result<std::vector<std::string>>());
    });
    fv::golden::for_each(kModule, "source_name", [](const fv::golden::Case& c) {
        const BatchItem item{c.arg<std::string>(0), std::nullopt};
        CHECK(item.source_name() == c.result<std::string>());
    });
}

TEST_CASE("golden batch_upload batch_context", "[golden]") {
    fv::golden::for_each(kModule, "batch_context", [](const fv::golden::Case& c) {
        const BatchUploadRequest req = request_from(c.args()[0]);
        const auto index = c.arg<std::optional<std::size_t>>(1);
        const BatchItem* item = index ? &req.items.at(*index) : nullptr;
        CHECK(req.batch_context(item) == c.result<std::optional<std::string>>());
    });
}

TEST_CASE("golden batch_upload result times", "[golden]") {
    fv::golden::for_each(kModule, "result_times", [](const fv::golden::Case& c) {
        BatchResult result;
        int index = 0;
        for (const auto& spec : c.args()[0]) {
            BatchItemResult item;
            item.item.audio_path = std::format("/a/{}.wav", index++);
            if (spec.contains("duration") && !spec.at("duration").is_null()) {
                item.item.duration_seconds = fv::golden::decode<double>(spec.at("duration"));
            }
            item.elapsed_s = fv::golden::decode<double>(spec.value("elapsed", fv::Json(0.0)));
            item.cleanup_elapsed_s =
                fv::golden::decode<double>(spec.value("cleanup_elapsed", fv::Json(0.0)));
            if (spec.contains("file_size") && !spec.at("file_size").is_null()) {
                item.file_size = fv::golden::decode<std::int64_t>(spec.at("file_size"));
            }
            result.items.push_back(item);
        }
        result.total_elapsed_s = c.arg<double>(1);
        result.transcription_time_s = c.arg<double>(2);
        result.cleanup_time_s = c.arg<double>(3);
        const fv::Json expected = c.result<fv::Json>();
        // Exact: Python's compensated float sum is reproduced bit for bit.
        CHECK(result.total_duration_seconds() ==
              fv::golden::decode<double>(expected.at("total_duration_seconds")));
        CHECK(result.total_file_size() ==
              fv::golden::decode<std::int64_t>(expected.at("total_file_size")));
        CHECK(result.effective_transcription_time_s() ==
              fv::golden::decode<double>(expected.at("effective_transcription_time_s")));
        CHECK(result.effective_cleanup_time_s() ==
              fv::golden::decode<double>(expected.at("effective_cleanup_time_s")));
    });
}

TEST_CASE("golden batch_upload text helpers", "[golden]") {
    fv::golden::for_each(kModule, "compose_batch_cleanup_prompt", [](const fv::golden::Case& c) {
        CHECK(compose_batch_cleanup_prompt(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
    fv::golden::for_each(kModule, "join_raw_parts", [](const fv::golden::Case& c) {
        CHECK(join_raw_parts(c.arg<std::vector<std::string>>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "format_batch_transcript", [](const fv::golden::Case& c) {
        std::vector<std::pair<std::string, std::string>> sections;
        for (const auto& pair : c.args()[0]) {
            sections.emplace_back(pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
        }
        CHECK(format_batch_transcript(sections) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "batch_source_name", [](const fv::golden::Case& c) {
        const auto names = c.arg<std::vector<std::string>>(0);
        int limit = fv::dictation::TRANSCRIPT_BATCH_SOURCE_NAME_MAX_CHARS;
        if (c.arg_count() > 1) {
            limit = c.arg<int>(1);
        } else if (c.kwargs().contains("limit")) {
            limit = fv::golden::decode<int>(c.kwargs().at("limit"));
        }
        CHECK(batch_source_name(names, limit) == c.result<std::string>());
    });
}
