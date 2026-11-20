#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "dictation/batch.h"
#include "dictation/uploads.h"
#include "golden/golden.h"

// Replays the batch builders of tests/golden/services.runtime.transcription.json
// (tools/parity/cases/services.runtime.transcription.py): a file is {"path", "text",
// "raw_text", "error"}, a result {"relation", "custom_combine", "items", "combined_text",
// "combined_raw_text", "cleanup_error"}.

using namespace fv::dictation;
namespace golden = fv::golden;
using fv::Json;

namespace {

constexpr std::string_view kTranscription = "services.runtime.transcription";

std::optional<std::string> optional_string(const Json& fields, const char* key) {
    const auto found = fields.find(key);
    if (found == fields.end() || found->is_null()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

BatchItemResult item_result(const Json& fields) {
    BatchItemResult result;
    result.item.audio_path = fields.at("path").get<std::string>();
    result.text = fields.value("text", std::string());
    result.raw_text = optional_string(fields, "raw_text");
    result.error = optional_string(fields, "error");
    return result;
}

BatchResult batch_result(const Json& fields) {
    BatchResult result;
    for (const Json& item : fields.at("items")) {
        result.items.push_back(item_result(item));
        result.request.items.push_back(result.items.back().item);
    }
    result.request.relation =
        batch_relation_from(fields.value("relation", std::string("separate"))).value();
    result.request.custom_combine = fields.value("custom_combine", false);
    result.combined_text = optional_string(fields, "combined_text");
    result.combined_raw_text = optional_string(fields, "combined_raw_text");
    result.cleanup_error = optional_string(fields, "cleanup_error");
    return result;
}

} // namespace

TEST_CASE("golden: _batch_line", "[dictation][golden]") {
    golden::for_each(kTranscription, "_batch_line", [](const golden::Case& c) {
        CHECK(batch_line(item_result(c.arg<Json>(0)), c.arg<bool>(1)) == c.result<std::string>());
    });
}

TEST_CASE("golden: _batch_display", "[dictation][golden]") {
    golden::for_each(kTranscription, "_batch_display", [](const golden::Case& c) {
        const auto [display, raw] = batch_display(batch_result(c.arg<Json>(0)), c.arg<bool>(1));
        const Json expected = c.result<Json>();
        CHECK(display == expected.at(0).get<std::string>());
        CHECK(raw == golden::decode<std::optional<std::string>>(expected.at(1)));
    });
}

TEST_CASE("golden: _batch_status", "[dictation][golden]") {
    golden::for_each(kTranscription, "_batch_status", [](const golden::Case& c) {
        CHECK(batch_status(batch_result(c.arg<Json>(0)), c.arg<std::size_t>(1), c.arg<bool>(2)) ==
              c.result<std::string>());
    });
}
