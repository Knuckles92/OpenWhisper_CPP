// The golden vectors of ui_qt.dialogs.guided_setup
// (tools/parity/cases/ui_qt.dialogs.guided_setup.py): shortcut_help and _on_number_pad as they are,
// _refresh_speech's lines and visibility over the speech states, and model_download_progress's
// text.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include "base/json.h"
#include "golden/golden.h"
#include "hotkeys/testing/vocabulary.h"
#include "ui_setup/guided_setup_dialog.h"

namespace {

using fv::Json;
namespace golden = fv::golden;

} // namespace

TEST_CASE("shortcut_help follows the trigger mode, the latch, the number pad and side buttons",
          "[golden]") {
    // The vectors were made with the Windows hook, whose side-button note differs from pynput's.
    const fv::hotkeys::testing::ScopedVocabulary windows(fv::hotkeys::Vocabulary::Windows);
    golden::for_each("ui_qt.dialogs.guided_setup", "shortcut_help", [](const golden::Case& c) {
        CHECK(fv::shortcut_help(c.arg<std::string>(0), c.arg<Json>(1)) == c.result<std::string>());
    });
}

TEST_CASE("_on_number_pad reads kp keys alone and after modifiers", "[golden]") {
    golden::for_each("ui_qt.dialogs.guided_setup", "_on_number_pad", [](const golden::Case& c) {
        CHECK(fv::on_number_pad(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("_refresh_speech shows ready, missing, failed and downloading states", "[golden]") {
    golden::for_each("ui_qt.dialogs.guided_setup", "_refresh_speech", [](const golden::Case& c) {
        std::optional<fv::SpeechProgress> progress;
        if (const Json raw = c.arg<Json>(3); !raw.is_null()) {
            progress =
                fv::SpeechProgress{raw.at(0).get<std::string>(), raw.at(1).get<std::int64_t>(),
                                   raw.at(2).get<std::int64_t>()};
        }
        const fv::SpeechStatus status =
            fv::speech_status(c.arg<std::string>(0), c.arg<std::string>(1),
                              c.arg<std::optional<bool>>(2), progress, c.arg<std::string>(4));
        const Json expected = c.result<Json>();
        CHECK(status.summary_line == expected.at("summary_line").get<std::string>());
        if (expected.at("ready_note").is_null()) {
            CHECK_FALSE(status.ready_note.has_value());
        } else {
            CHECK(status.ready_note == expected.at("ready_note").get<std::string>());
        }
        CHECK(status.tone == expected.at("tone").get<std::string>());
        CHECK(status.tone == expected.at("ready_note_tone").get<std::string>());
        CHECK(status.progress_visible == expected.at("progress_visible").get<bool>());
        CHECK(status.progress_visible == expected.at("ready_progress_visible").get<bool>());
        if (expected.at("bar_maximum").is_null()) {
            CHECK_FALSE(progress.has_value());
        } else {
            CHECK(status.bar_maximum == expected.at("bar_maximum").get<int>());
        }
        if (expected.at("bar_value").is_null()) {
            CHECK_FALSE(status.bar_value.has_value());
        } else {
            CHECK(status.bar_value == expected.at("bar_value").get<int>());
        }
        CHECK(status.record_download_visible == expected.at("record_download_visible").get<bool>());
        CHECK(status.ready_download_visible == expected.at("ready_download_visible").get<bool>());
        CHECK(status.ready_row_visible == expected.at("ready_row_visible").get<bool>());
        CHECK(status.ready_title == expected.at("ready_title").get<std::string>());
        CHECK(status.ready_lede == expected.at("ready_lede").get<std::string>());
    });
}

TEST_CASE("model_download_progress names the sizes once the total is known", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.guided_setup", "model_download_progress", [](const golden::Case& c) {
            const Json expected = c.result<Json>();
            CHECK(fv::download_progress_text(c.arg<std::int64_t>(0), c.arg<std::int64_t>(1)) ==
                  expected.at(0).get<std::string>());
        });
}
