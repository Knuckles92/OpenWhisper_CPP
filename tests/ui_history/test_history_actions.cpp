// Paste or copy the original of the last dictation, from the shortcut and the tray
// (tests/test_personalize_s9_paste_original.py,
// tests/test_personalize_fix_f1_tray_copy_original.py).
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <QString>

#include "history_ui_support.h"
#include "ui_history/history_actions.h"

namespace actions = fv::history_actions;
using fvtest::LocalHistory;

namespace {

// The FakeUI of both Python files.
class FakeUi : public fv::HistoryActionsUi {
public:
    void setStatus(const QString& status) override { statuses.push_back(status); }
    bool canPasteNow() const override { return hasPaste; }
    bool pasteTextNow(const QString& text) override {
        pasted.push_back(text);
        return pastes;
    }
    void refreshHistory() override { ++refreshed; }
    bool copyToClipboard(const QString& text) override {
        copied.push_back(text);
        return copies;
    }

    std::vector<QString> statuses;
    std::vector<QString> pasted;
    std::vector<QString> copied;
    int refreshed = 0;
    bool pastes = true;
    bool copies = true;
    bool hasPaste = true;
};

QString text(const char* utf8) {
    return QString::fromUtf8(utf8);
}

// _dictation: a live dictation saved through History.
fv::HistoryEntry dictation(LocalHistory& history, std::string spoken,
                           std::optional<std::string> raw = std::nullopt) {
    fv::AddEntry fields;
    fields.text = std::move(spoken);
    fields.raw_text = std::move(raw);
    fields.model = "base";
    fields.source_name = "Quick Record";
    fields.columns = {{"entry_kind", "dictation"}};
    return history.add(fields);
}

void paste(FakeUi& ui, LocalHistory& history) {
    fvtest::offGui([&] { fv::paste_last_original(ui, *history.store()); });
}

void copy(FakeUi& ui, LocalHistory& history) {
    fvtest::offGui([&] { fv::copy_last_original(ui, *history.store()); });
}

} // namespace

// from tests/test_personalize_s9_paste_original.py::test_nothing_dictated_yet
TEST_CASE("nothing dictated yet", "[test_personalize_s9_paste_original]") {
    LocalHistory history;
    FakeUi ui;
    paste(ui, history);
    CHECK(ui.statuses == std::vector<QString>{text(actions::kNothingYet)});
    CHECK(ui.pasted.empty());
}

// from
// tests/test_personalize_s9_paste_original.py::test_the_last_dictations_original_is_pasted_and_kept
TEST_CASE("the last dictations original is pasted and kept",
          "[test_personalize_s9_paste_original]") {
    LocalHistory history;
    const fv::HistoryEntry entry = dictation(history, "Hello, world.", "um hello world");
    FakeUi ui;

    paste(ui, history);

    CHECK(ui.pasted == std::vector<QString>{QStringLiteral("um hello world")});
    CHECK(ui.statuses == std::vector<QString>{text(actions::kPasted)});
    CHECK(ui.refreshed == 1);
    const fv::HistoryEntry stored = *history.get(entry.id);
    CHECK(stored.text == "um hello world");
    CHECK(stored.cleaned_text == "Hello, world.");

    paste(ui, history);
    CHECK(ui.pasted == std::vector<QString>(2, QStringLiteral("um hello world")));
    CHECK(history.get(entry.id)->text == "um hello world");
}

// from
// tests/test_personalize_s9_paste_original.py::test_an_unchanged_last_dictation_never_reaches_back_to_an_older_one
TEST_CASE("an unchanged last dictation never reaches back to an older one",
          "[test_personalize_s9_paste_original]") {
    LocalHistory history;
    dictation(history, "Older, cleaned.", "older cleaned");
    dictation(history, "as heard");
    FakeUi ui;
    paste(ui, history);
    CHECK(ui.statuses == std::vector<QString>{text(actions::kNotEdited)});
    CHECK(ui.pasted.empty());
}

// from tests/test_personalize_s9_paste_original.py::test_uploads_and_rewrites_after_it_are_skipped
TEST_CASE("uploads and rewrites after it are skipped", "[test_personalize_s9_paste_original]") {
    LocalHistory history;
    dictation(history, "Dictated.", "dictated um");
    fv::AddEntry file;
    file.text = "a file";
    file.model = "base";
    file.source_name = "talk.mp3";
    file.columns = {{"entry_kind", "file"}};
    history.add(file);
    fv::AddEntry command;
    command.text = "Formal.";
    command.raw_text = "formal pls";
    command.model = "base";
    command.source_name = "Quick Record";
    command.columns = {{"entry_kind", "command"}};
    history.add(command);
    FakeUi ui;
    paste(ui, history);
    CHECK(ui.pasted == std::vector<QString>{QStringLiteral("dictated um")});
}

// from
// tests/test_personalize_s9_paste_original.py::test_a_refused_paste_leaves_the_entry_and_the_status_alone
TEST_CASE("a refused paste leaves the entry and the status alone",
          "[test_personalize_s9_paste_original]") {
    LocalHistory history;
    const fv::HistoryEntry entry = dictation(history, "Hello, world.", "um hello world");
    FakeUi ui;
    ui.pastes = false;
    paste(ui, history);
    CHECK(ui.pasted == std::vector<QString>{QStringLiteral("um hello world")});
    CHECK(ui.statuses.empty());
    CHECK(history.get(entry.id)->text == "Hello, world.");
}

// from tests/test_personalize_s9_paste_original.py::test_without_a_paste_hook_it_says_so
TEST_CASE("without a paste hook it says so", "[test_personalize_s9_paste_original]") {
    LocalHistory history;
    dictation(history, "Hello, world.", "um hello world");
    FakeUi ui;
    ui.hasPaste = false;
    paste(ui, history);
    CHECK(ui.statuses == std::vector<QString>{text(actions::kUnavailable)});
}

// from
// tests/test_personalize_fix_f1_tray_copy_original.py::test_copy_puts_the_original_on_the_clipboard_and_keeps_the_version
TEST_CASE("copy puts the original on the clipboard and keeps the version",
          "[test_personalize_fix_f1_tray_copy_original]") {
    LocalHistory history;
    const fv::HistoryEntry entry = dictation(history, "Hello, world.", "um hello world");
    FakeUi ui;

    copy(ui, history);

    CHECK(ui.copied == std::vector<QString>{QStringLiteral("um hello world")});
    CHECK(ui.pasted.empty());
    CHECK(ui.statuses == std::vector<QString>{text(actions::kCopied)});
    CHECK(text(actions::kCopied).contains(QStringLiteral("Ctrl+V")));
    CHECK(history.get(entry.id)->text == "Hello, world.");
}

// from tests/test_personalize_fix_f1_tray_copy_original.py::test_a_failed_copy_says_so
TEST_CASE("a failed copy says so", "[test_personalize_fix_f1_tray_copy_original]") {
    LocalHistory history;
    const fv::HistoryEntry entry = dictation(history, "Hello, world.", "um hello world");
    FakeUi ui;
    ui.copies = false;

    copy(ui, history);

    CHECK(ui.statuses == std::vector<QString>{text(actions::kCopyFailed)});
    CHECK(history.get(entry.id)->text == "Hello, world.");
}

// from tests/test_personalize_fix_f1_tray_copy_original.py::test_copy_with_nothing_to_copy
TEST_CASE("copy with nothing to copy", "[test_personalize_fix_f1_tray_copy_original]") {
    LocalHistory history;
    FakeUi ui;
    copy(ui, history);
    dictation(history, "as heard");
    copy(ui, history);

    CHECK(ui.statuses ==
          std::vector<QString>{text(actions::kNothingToCopy), text(actions::kNotEdited)});
    CHECK(ui.copied.empty());
    CHECK(ui.pasted.empty());
}

TEST_CASE("the notice says what was done", "[unit]") {
    LocalHistory history;
    dictation(history, "Hello, world.", "um hello world");
    struct NoticeUi final : FakeUi {
        void showNotice(const QString& notice, bool done) override {
            notices.emplace_back(notice, done);
        }
        std::vector<std::pair<QString, bool>> notices;
    };
    NoticeUi ui;
    paste(ui, history);
    copy(ui, history);
    CHECK(ui.notices == std::vector<std::pair<QString, bool>>{
                            {QStringLiteral("Original pasted"), true},
                            {QStringLiteral("Copied — paste with Ctrl+V"), true}});
}
