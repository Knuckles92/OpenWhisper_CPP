// tests/test_personalize_s9_history_edit.py: choosing between an entry's original and AI
// versions (the export and agents test is M2's).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "history/entry.h"
#include "history/manager.h"
#include "history_test_support.h"
#include "pyport/errors.h"

using fv::AI_VERSION;
using fv::HistoryManager;
using fv::ORIGINAL_VERSION;
using fv::history_test::RecordingSync;
using fv::history_test::services_with;
using fv::history_test::TestDatabase;

namespace {

using Versions = std::tuple<std::string, std::optional<std::string>, std::optional<std::string>>;

Versions versions(const fv::HistoryEntry& entry) {
    return {entry.text, entry.raw_text, entry.cleaned_text};
}

// The `sync` fixture and history_manager on a fresh database.
struct Fixture {
    TestDatabase db;
    RecordingSync sync;
    HistoryManager manager{db.database(), db.recordings(), fv::RecordingRetention{},
                           services_with(&sync)};

    fv::HistoryEntry cleaned(std::string text = "Hello, world.", std::string raw = "hello world um",
                             fv::ContextFields columns = {}) {
        fv::AddEntry fields;
        fields.text = std::move(text);
        fields.model = "base";
        fields.raw_text = std::move(raw);
        fields.columns = std::move(columns);
        return manager.add_entry(fields).value();
    }
    fv::HistoryEntry plain(std::string text, fv::ContextFields columns = {}) {
        fv::AddEntry fields;
        fields.text = std::move(text);
        fields.model = "base";
        fields.columns = std::move(columns);
        return manager.add_entry(fields).value();
    }
};

using Edits = std::vector<std::pair<std::string, std::string>>;

} // namespace

// from tests/test_personalize_s9_history_edit.py::test_saving_an_ai_edit_keeps_both_versions
TEST_CASE("test_saving_an_ai_edit_keeps_both_versions", "[test_personalize_s9_history_edit]") {
    Fixture f;
    const auto entry = f.cleaned("Hello, world.", "hello world um", {{"entry_kind", "dictation"}});

    const auto stored = f.manager.get_entry(entry.id);
    REQUIRE(stored);
    CHECK(versions(*stored) == Versions{"Hello, world.", "hello world um", "Hello, world."});
    CHECK(fv::entry_version(*stored) == AI_VERSION);
    const auto plain = f.plain("as heard");
    CHECK_FALSE(f.manager.get_entry(plain.id)->cleaned_text);
    CHECK(fv::entry_version(plain) == "");
}

// from
// tests/test_personalize_s9_history_edit.py::test_choosing_the_original_and_back_never_swaps_fields
TEST_CASE("test_choosing_the_original_and_back_never_swaps_fields",
          "[test_personalize_s9_history_edit]") {
    Fixture f;
    const auto entry = f.cleaned();

    const auto original = f.manager.use_version(entry.id, ORIGINAL_VERSION);
    REQUIRE(original);
    CHECK(versions(*original) == Versions{"hello world um", "hello world um", "Hello, world."});
    const auto stored = f.manager.get_entry(entry.id);
    REQUIRE(stored);
    CHECK(fv::entry_version(*stored) == ORIGINAL_VERSION);
    CHECK(fv::ai_text(*stored) == "Hello, world.");

    const auto ai = f.manager.use_version(entry.id, AI_VERSION);
    REQUIRE(ai);
    CHECK(versions(*ai) == Versions{"Hello, world.", "hello world um", "Hello, world."});
    CHECK(f.sync.edited == Edits{{"dictation", entry.id}, {"dictation", entry.id}});
}

// from
// tests/test_personalize_s9_history_edit.py::test_a_legacy_entry_fills_in_its_ai_version_on_the_first_undo
TEST_CASE("test_a_legacy_entry_fills_in_its_ai_version_on_the_first_undo",
          "[test_personalize_s9_history_edit]") {
    Fixture f;
    f.db.add("legacy", "Cleaned long ago.", "2026-01-01T09:00:00", "base", {}, "Quick Record",
             "cleaned long ago");
    const auto legacy = f.manager.get_entry("legacy");
    REQUIRE(legacy);
    CHECK_FALSE(legacy->cleaned_text);
    CHECK(fv::entry_version(*legacy) == AI_VERSION);
    CHECK(fv::ai_text(*legacy) == "Cleaned long ago.");

    f.manager.use_version("legacy", ORIGINAL_VERSION);
    const auto stored = f.manager.get_entry("legacy");
    CHECK(stored->text == "cleaned long ago");
    CHECK(stored->cleaned_text == "Cleaned long ago.");

    f.manager.use_version("legacy", AI_VERSION);
    CHECK(f.manager.get_entry("legacy")->text == "Cleaned long ago.");
}

// from
// tests/test_personalize_s9_history_edit.py::test_choosing_the_version_already_shown_changes_nothing
TEST_CASE("test_choosing_the_version_already_shown_changes_nothing",
          "[test_personalize_s9_history_edit]") {
    Fixture f;
    const auto entry = f.cleaned();
    CHECK(f.manager.use_version(entry.id, AI_VERSION)->text == "Hello, world.");
    CHECK(f.sync.edited.empty());
}

// from
// tests/test_personalize_s9_history_edit.py::test_only_this_computers_entries_with_an_ai_edit_can_change
TEST_CASE("test_only_this_computers_entries_with_an_ai_edit_can_change",
          "[test_personalize_s9_history_edit]") {
    Fixture f;
    f.db.add("laptop", "Theirs.", "2026-01-01T09:00:00+00:00", "base", {}, std::nullopt, "theirs");
    f.db.set_origin("laptop", "device-1");
    const auto plain = f.plain("no cleanup");

    CHECK_FALSE(f.manager.use_version("laptop", ORIGINAL_VERSION));
    CHECK(f.manager.get_entry("laptop")->text == "Theirs.");
    CHECK_FALSE(f.manager.use_version(plain.id, ORIGINAL_VERSION));
    CHECK_FALSE(f.manager.use_version("missing", ORIGINAL_VERSION));
    try {
        f.manager.use_version(plain.id, "raw");
        FAIL("expected ValueError");
    } catch (const fv::py::ValueError& error) {
        CHECK(std::string(error.what()) == "Unknown history entry version: 'raw'");
    }
    CHECK(f.sync.edited.empty());
}

// from tests/test_personalize_s9_history_edit.py::test_a_host_kept_entry_is_never_edited
TEST_CASE("test_a_host_kept_entry_is_never_edited", "[test_personalize_s9_history_edit]") {
    Fixture f;
    // remote_history_entry({...}): a host-kept entry as History shows it, never in this
    // computer's database (the conversion itself arrives with record sync).
    fv::HistoryEntry remote;
    remote.id = "on-host";
    remote.text = "Cleaned.";
    remote.raw_text = "cleaned";
    remote.timestamp = "2026-01-01T09:00:00+00:00";
    remote.model = "base";
    remote.stored_on = "devbox";

    CHECK(fv::entry_version(remote) == AI_VERSION);
    CHECK_FALSE(fv::is_local_entry(remote));
    CHECK_FALSE(f.manager.use_version("on-host", ORIGINAL_VERSION));
    CHECK(f.sync.edited.empty());
}

// from tests/test_personalize_s9_history_edit.py::test_the_database_only_edits_the_text_fields
TEST_CASE("test_the_database_only_edits_the_text_fields", "[test_personalize_s9_history_edit]") {
    TestDatabase db;
    auto& repository = db.repository();
    db.add("e1", "a", "2026-01-01T09:00:00+00:00");
    try {
        repository.update_history_entry("e1", {{"model", "other"}});
        FAIL("expected TypeError");
    } catch (const fv::py::TypeError& error) {
        CHECK(std::string(error.what()) == "Not editable history fields: model");
    }
    try {
        repository.update_history_entry("e1", {{"text", std::nullopt}});
        FAIL("expected TypeError");
    } catch (const fv::py::TypeError& error) {
        CHECK(std::string(error.what()) == "text can't be cleared");
    }
    CHECK_FALSE(repository.update_history_entry("missing", {{"text", "b"}}));
    CHECK(repository.update_history_entry("e1", {{"text", "b"}, {"cleaned_text", "c"}})->text ==
          "b");
    CHECK(repository.get_history_entry_by_id("e1")->cleaned_text == "c");
}

// from tests/test_personalize_s9_history_edit.py::test_search_finds_entries_by_app
TEST_CASE("test_search_finds_entries_by_app", "[test_personalize_s9_history_edit]") {
    Fixture f;
    f.plain("see you at noon", {{"app_name", "Slack"}});
    f.plain("quarterly numbers", {{"app_name", "Outlook"}});

    std::vector<std::string> texts;
    for (const auto& entry : f.manager.search("slack")) {
        texts.push_back(entry.text);
    }
    CHECK(texts == std::vector<std::string>{"see you at noon"});
}

// from
// tests/test_personalize_s9_history_edit.py::test_last_dictation_skips_files_rewrites_and_other_computers
TEST_CASE("test_last_dictation_skips_files_rewrites_and_other_computers",
          "[test_personalize_s9_history_edit]") {
    Fixture f;
    f.db.add("old", "old", "2026-01-01T09:00:00", "base", {}, "Quick Record · Email");
    f.db.add("file", "file", "2026-01-02T09:00:00+00:00", "base", {}, "talk.mp3");
    f.db.add("cmd", "cmd", "2026-01-03T09:00:00+00:00", "base", {{"entry_kind", "command"}},
             "Quick Record");
    f.db.add("theirs", "theirs", "2026-01-04T09:00:00+00:00", "base", {}, "Quick Record");
    f.db.set_origin("theirs", "device-1");

    CHECK(f.manager.last_dictation()->id == "old");
    CHECK(fv::kind_of(*f.manager.get_entry("file")) == "file");
    CHECK(fv::kind_of(*f.manager.get_entry("cmd")) == "command");
}
