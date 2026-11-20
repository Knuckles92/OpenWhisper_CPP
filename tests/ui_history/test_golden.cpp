// The History card's words against OpenWhisper's helpers
// (tests/golden/ui_qt.widgets.history_sidebar.json, ui_qt.widgets.history_playback.json), and
// OpenWhisper_CPP's own badge for its Parakeet model strings.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_text.h"

namespace golden = fv::golden;
using fv::HistoryEntry;

namespace {

constexpr char kModule[] = "ui_qt.widgets.history_sidebar";

std::optional<std::string> text(const fv::Json& fields, const char* name) {
    const auto found = fields.find(name);
    if (found == fields.end() || found->is_null()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

// The cases' entry dicts: TranscriptionHistory columns, missing ones None.
HistoryEntry entryFrom(const fv::Json& fields) {
    HistoryEntry entry;
    entry.id = text(fields, "id").value_or("");
    entry.text = text(fields, "text").value_or("");
    entry.raw_text = text(fields, "raw_text");
    entry.model = text(fields, "model").value_or("");
    entry.cleanup_provider = text(fields, "cleanup_provider");
    entry.cleanup_model = text(fields, "cleanup_model");
    entry.source_name = text(fields, "source_name");
    entry.origin_device_name = text(fields, "origin_device_name");
    entry.cleanup_level = text(fields, "cleanup_level");
    entry.entry_kind = text(fields, "entry_kind");
    entry.cleaned_text = text(fields, "cleaned_text");
    entry.stored_on = text(fields, "stored_on");
    entry.also_on = text(fields, "also_on");
    return entry;
}

} // namespace

TEST_CASE("golden _format_model_name", "[golden]") {
    golden::for_each(kModule, "_format_model_name", [](const golden::Case& c) {
        CHECK(fv::format_model_name(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden _format_cleanup_info", "[golden]") {
    golden::for_each(kModule, "_format_cleanup_info", [](const golden::Case& c) {
        CHECK(fv::format_cleanup_info(entryFrom(c.arg<fv::Json>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden _cleanup_tooltip", "[golden]") {
    golden::for_each(kModule, "_cleanup_tooltip", [](const golden::Case& c) {
        CHECK(fv::cleanup_tooltip(entryFrom(c.arg<fv::Json>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden _entry_was_cleaned", "[golden]") {
    golden::for_each(kModule, "_entry_was_cleaned", [](const golden::Case& c) {
        CHECK(fv::entry_was_cleaned(entryFrom(c.arg<fv::Json>(0))) == c.result<bool>());
    });
}

TEST_CASE("golden _kind_label", "[golden]") {
    golden::for_each(kModule, "_kind_label", [](const golden::Case& c) {
        CHECK(fv::kind_label(entryFrom(c.arg<fv::Json>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden _location_chip", "[golden]") {
    golden::for_each(kModule, "_location_chip", [](const golden::Case& c) {
        const auto expected = c.result<std::vector<std::string>>();
        REQUIRE(expected.size() == 2);
        const fv::LocationChip chip = fv::location_chip(entryFrom(c.arg<fv::Json>(0)));
        CHECK(chip.text == expected[0]);
        CHECK(chip.tooltip == expected[1]);
    });
}

TEST_CASE("golden _failed_recording_hint", "[golden]") {
    golden::for_each(kModule, "_failed_recording_hint", [](const golden::Case& c) {
        CHECK(fv::failed_recording_hint(c.arg<bool>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden clock", "[golden]") {
    golden::for_each("ui_qt.widgets.history_playback", "clock", [](const golden::Case& c) {
        CHECK(fv::playback::clock(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("a Parakeet model string shows its model label", "[unit]") {
    // TranscriptionRuntime saves "parakeet (<model label> | <device>)".
    CHECK(fv::format_model_name("parakeet (Parakeet TDT 0.6B v3 | cuda)") ==
          "Parakeet TDT 0.6B v3");
    CHECK(fv::format_model_name("parakeet (Orukeet TDT 0.6B | cpu)") == "Orukeet TDT 0.6B");
    CHECK(fv::format_model_name("  parakeet  ") == "Parakeet");
    CHECK(fv::format_model_name("parakeet ()") == "Parakeet");
    // Only the bare engine key maps; anything else keeps OpenWhisper's rule.
    CHECK(fv::format_model_name("parakeet-v3 (x)") == "parakeet-v3 · x");
}

TEST_CASE("the card fingerprint changes with what the card shows", "[unit]") {
    HistoryEntry entry;
    entry.id = "e";
    entry.text = "hello";
    entry.timestamp = "2026-01-01T00:00:00";
    entry.model = "base";
    entry.audio_file = "a.wav";
    const fv::Json base = fv::card_fingerprint(entry, true);
    CHECK(base == fv::card_fingerprint(entry, true));
    CHECK(base.at("cleanup_display") == "Cleaned");
    CHECK(base.at("audio_available") == true);
    // The recording removed by retention changes the card (Play turns off).
    CHECK(base != fv::card_fingerprint(entry, false));
    HistoryEntry edited = entry;
    edited.text = "hello!";
    CHECK(base != fv::card_fingerprint(edited, true));
    // A host-kept entry has no local recording to check.
    HistoryEntry remote = entry;
    remote.stored_on = "devbox";
    CHECK_FALSE(fv::card_fingerprint(remote, true).contains("audio_available"));
}
