#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "dictation/messages.h"
#include "golden/golden.h"
#include "settings/keys.h"
#include "walkthrough/session.h"
#include "walkthrough_support.h"

// Replays the walkthrough statuses of tests/golden/services.runtime.transcription.json
// (tools/parity/cases/services.runtime.transcription.py, the M6.11 section): _copy_walkthrough
// for every copy format, auto-paste, copy outcome, write-up error and lost screenshots, and
// _deliver_walkthrough's statuses for a walkthrough without a document. Each case follows the
// runtime's own steps (copy_walkthrough, deliver_walkthrough) over the pure builders.

using namespace fv::dictation;
using fv::Json;
namespace golden = fv::golden;

namespace {

constexpr std::string_view kTranscription = "services.runtime.transcription";

Json names_and_text(const std::vector<std::filesystem::path>& files, const std::string& text) {
    Json names = Json::array();
    for (const auto& file : files) {
        names.push_back(fv::path_to_utf8(file.filename()));
    }
    return Json::array({names, !text.empty()});
}

} // namespace

TEST_CASE("golden: a finished walkthrough's copy status", "[dictation][golden]") {
    golden::for_each(kTranscription, "_copy_walkthrough", [](const golden::Case& c) {
        const Json settings{{"walkthrough_copy_format", c.arg<std::string>(0)},
                            {"auto_paste", c.arg<bool>(1)}};
        const bool copies = c.arg<bool>(2);
        fv::walkthrough::WalkthroughResult result;
        result.folder = fv::path_from_utf8("C:/captures/2026-10-21_14-32-00");
        result.written_up = c.arg<bool>(4);
        result.pdf = result.folder / (result.written_up ? "write-up.pdf" : "walkthrough.pdf");
        result.markdown = result.folder / (result.written_up ? "write-up.md" : "walkthrough.md");
        result.clipboard_text = "# Walkthrough";
        result.history_text = "words";
        result.shots = c.arg<int>(3);
        result.duration_s = 12.0;
        result.write_up_error = c.arg<std::string>(5);
        result.lost_shots = c.arg<int>(6);

        const std::string format = fv::keys::walkthrough_copy_format.read(settings);
        const bool auto_paste = format == "agent_task" && fv::keys::auto_paste.read(settings);
        const auto [files, text] = result.clipboard(format);
        WalkthroughCopy copy;
        copy.name = result.name();
        copy.shots = result.shots;
        copy.write_up_error = result.write_up_error;
        copy.copy_format = format;
        copy.auto_paste = auto_paste;
        copy.copied = copies;
        copy.with_text = !text.empty();
        copy.lost_shots = result.lost_shots;

        const Json actual{{"statuses", Json::array({walkthrough_copy_status(copy)})},
                          {"copied", Json::array({names_and_text(files, text)})},
                          {"ready", Json::array({copies})},
                          {"pasted", auto_paste ? 1 : 0}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden: a finished walkthrough's copy through the runtime", "[dictation][golden]") {
    // The same cases through TranscriptionRuntime's own copy_walkthrough (reached by
    // on_walkthrough_written_up, as a write-up that came back): the settings it reads, what it
    // copies, the card, the status and the paste. One runtime for every case, in a plain loop:
    // golden::for_each's sections would build it again for each one.
    fv::dictation::testing::WalkthroughRig::Options options;
    options.front = fv::dictation::testing::chat_window();
    fv::dictation::testing::WalkthroughRig w(options);
    const Json& cases = golden::load(kTranscription).at("functions").at("_copy_walkthrough");
    REQUIRE(cases.size() == 144);
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const golden::Case c("_copy_walkthrough", index, cases[index], 0.0);
        INFO(c.label());
        w.rig.settings.update(Json{{"walkthrough_copy_format", c.arg<std::string>(0)},
                                   {"auto_paste", c.arg<bool>(1)}});
        w.delivery.copies = c.arg<bool>(2);
        fv::walkthrough::WalkthroughResult result;
        result.folder = fv::path_from_utf8("C:/captures/2026-10-21_14-32-00");
        result.written_up = c.arg<bool>(4);
        result.pdf = result.folder / (result.written_up ? "write-up.pdf" : "walkthrough.pdf");
        result.markdown = result.folder / (result.written_up ? "write-up.md" : "walkthrough.md");
        result.clipboard_text = "# Walkthrough";
        result.history_text = "words";
        result.shots = c.arg<int>(3);
        result.duration_s = 12.0;
        result.write_up_error = c.arg<std::string>(5);
        result.lost_shots = c.arg<int>(6);
        const std::size_t statuses = w.rig.statuses().size();
        const std::size_t copied = w.delivery.copied().size();
        const std::size_t ready = w.delivery.ready().size();
        const std::size_t pasted = w.delivery.pasted().size();

        w.runtime().on_walkthrough_written_up(result);

        Json actual{{"statuses", Json::array()},
                    {"copied", Json::array()},
                    {"ready", Json::array()},
                    {"pasted", static_cast<int>(w.delivery.pasted().size() - pasted)}};
        const auto all_statuses = w.rig.statuses();
        for (std::size_t i = statuses; i < all_statuses.size(); ++i) {
            actual["statuses"].push_back(all_statuses[i]);
        }
        const auto all_copied = w.delivery.copied();
        for (std::size_t i = copied; i < all_copied.size(); ++i) {
            actual["copied"].push_back(names_and_text(all_copied[i].first, all_copied[i].second));
        }
        const auto all_ready = w.delivery.ready();
        for (std::size_t i = ready; i < all_ready.size(); ++i) {
            actual["ready"].push_back(all_ready[i].second);
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    }
}

TEST_CASE("golden: a finished walkthrough without its document", "[dictation][golden]") {
    golden::for_each(kTranscription, "_deliver_walkthrough", [](const golden::Case& c) {
        const Json error = c.arg<Json>(0);
        const std::string document_error = error.is_null() ? "" : error.get<std::string>();
        const std::string transcript = c.arg<std::string>(1);
        const bool copies = c.arg<bool>(2);

        const bool copied = !transcript.empty() && copies;
        const std::string status =
            walkthrough_missing_status(document_error, copied, !transcript.empty());
        Json copies_made = Json::array();
        if (!transcript.empty()) {
            copies_made.push_back(Json::array({Json::array(), true}));
        }
        const Json actual{{"statuses", Json::array({status})},
                          {"notices", Json::array({status})},
                          {"copied", copies_made},
                          {"dismissed", 1}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}
