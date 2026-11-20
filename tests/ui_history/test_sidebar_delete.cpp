// Confirmation before deleting from a card's menu (tests/test_history_sidebar_delete.py).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>

#include "history_ui_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_history/history_sidebar.h"

using fv::HistorySidebar;
using fvtest::FakeStore;
using fvtest::Io;

namespace {

using Deletes = std::vector<std::pair<std::string, bool>>;

struct DeleteFixture {
    explicit DeleteFixture(bool confirm = true)
        : settings(fv::Json{{"confirm_history_entry_delete", confirm}}),
          sidebar(store, io.executor(), &settings) {
        QObject::connect(&sidebar, &HistorySidebar::entryDeleted,
                         [this](const QString& id) { deleted.push_back(id); });
    }

    // _asked: the user gives `answer`, after `before` had its say in the box.
    void answer(QMessageBox::StandardButton answer, std::function<void(QMessageBox&)> before = {}) {
        sidebar.runMessageBox = [this, answer, before](QMessageBox& box) {
            ++asked;
            if (before) {
                before(box);
            }
            return int(answer);
        };
    }

    void requestDelete() {
        sidebar.onDeleteRequested(QStringLiteral("entry-test-id"));
        io.drain();
    }

    std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
    Io io;
    fv::MemorySettingsStore settings;
    HistorySidebar sidebar;
    std::vector<QString> deleted;
    int asked = 0;
};

} // namespace

// from
// tests/test_history_sidebar_delete.py::TestHistorySidebarDelete::test_delete_is_canceled_when_confirmation_is_rejected
TEST_CASE("delete is canceled when confirmation is rejected",
          "[test_history_sidebar_delete][widget]") {
    DeleteFixture fixture;
    fixture.answer(QMessageBox::No);

    fixture.requestDelete();

    CHECK(fixture.asked == 1);
    CHECK(fixture.store->deletes().empty());
}

// from
// tests/test_history_sidebar_delete.py::TestHistorySidebarDelete::test_delete_proceeds_after_confirmation
TEST_CASE("delete proceeds after confirmation", "[test_history_sidebar_delete][widget]") {
    DeleteFixture fixture;
    QString title;
    QString question;
    QString detail;
    bool audioOffered = true;
    fixture.answer(QMessageBox::Yes, [&](QMessageBox& box) {
        title = box.windowTitle();
        question = box.text();
        detail = box.informativeText();
        audioOffered = box.findChild<QComboBox*>() != nullptr;
        CHECK(box.icon() == QMessageBox::Warning);
        CHECK(box.defaultButton() == box.button(QMessageBox::No));
        REQUIRE(box.checkBox() != nullptr);
        CHECK(box.checkBox()->text() == QStringLiteral("Don't ask me again"));
    });

    fixture.requestDelete();

    CHECK(fixture.store->deletes() == Deletes{{"entry-test-id", false}});
    CHECK(fixture.deleted == std::vector<QString>{QStringLiteral("entry-test-id")});
    CHECK(title == QStringLiteral("Delete History Entry"));
    CHECK(question == QStringLiteral("Delete this transcription from history?"));
    CHECK(detail == QStringLiteral("This cannot be undone."));
    CHECK_FALSE(audioOffered);
}

// from
// tests/test_history_sidebar_delete.py::TestHistorySidebarDelete::test_audio_file_can_be_deleted_with_entry
TEST_CASE("audio file can be deleted with entry", "[test_history_sidebar_delete][widget]") {
    DeleteFixture fixture;
    fv::HistoryEntry entry = fvtest::makeEntry("hello");
    entry.id = "entry-test-id";
    entry.audio_file = "recording.wav";
    fixture.store->entries[entry.id] = entry;
    fixture.store->recordings["recording.wav"] =
        std::filesystem::path(LR"(C:\recordings\recording.wav)");
    QString label;
    QStringList choices;
    fixture.answer(QMessageBox::Yes, [&](QMessageBox& box) {
        auto* combo = box.findChild<QComboBox*>();
        REQUIRE(combo != nullptr);
        for (int i = 0; i < combo->count(); ++i) {
            choices << combo->itemText(i);
        }
        for (const QLabel* each : box.findChildren<QLabel*>()) {
            if (each->text() == QStringLiteral("Delete saved audio file too?")) {
                label = each->text();
            }
        }
        combo->setCurrentIndex(1);
    });

    fixture.requestDelete();

    CHECK(fixture.store->deletes() == Deletes{{"entry-test-id", true}});
    CHECK(label == QStringLiteral("Delete saved audio file too?"));
    CHECK(choices == QStringList{QStringLiteral("No — keep the audio file"),
                                 QStringLiteral("Yes — permanently delete it")});
}

// from
// tests/test_history_sidebar_delete.py::TestHistorySidebarDelete::test_dont_ask_again_preference_is_saved
TEST_CASE("dont ask again preference is saved", "[test_history_sidebar_delete][widget]") {
    DeleteFixture fixture;
    fixture.answer(QMessageBox::Yes, [](QMessageBox& box) { box.checkBox()->setChecked(true); });

    fixture.requestDelete();

    CHECK(fixture.settings.contents().value("confirm_history_entry_delete", true) == false);
    CHECK(fixture.store->deletes() == Deletes{{"entry-test-id", false}});
}

// from
// tests/test_history_sidebar_delete.py::TestHistorySidebarDelete::test_saved_opt_out_skips_confirmation
TEST_CASE("saved opt out skips confirmation", "[test_history_sidebar_delete][widget]") {
    DeleteFixture fixture(false);
    fixture.answer(QMessageBox::Yes);

    fixture.requestDelete();

    CHECK(fixture.asked == 0);
    CHECK(fixture.store->deletes() == Deletes{{"entry-test-id", false}});
}

TEST_CASE("a copy also kept on the host is named in the confirmation", "[widget]") {
    DeleteFixture fixture;
    fv::HistoryEntry entry = fvtest::makeEntry("hello");
    entry.id = "entry-test-id";
    entry.also_on = "devbox";
    fixture.sidebar.applyHistoryResults(fixture.sidebar.loadGeneration(), QString(), {entry},
                                        QString());
    QString detail;
    fixture.answer(QMessageBox::No,
                   [&detail](QMessageBox& box) { detail = box.informativeText(); });

    fixture.requestDelete();

    CHECK(detail == QStringLiteral("This cannot be undone. Its copy on devbox is kept."));
    CHECK(fixture.store->deletes().empty());
}

TEST_CASE("a settings file that cannot be read still asks", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    Io io;
    fv::MemorySettingsStore settings;
    settings.file().set_bytes("not json");
    HistorySidebar sidebar(store, io.executor(), &settings);
    int asked = 0;
    sidebar.runMessageBox = [&asked](QMessageBox&) {
        ++asked;
        return int(QMessageBox::No);
    };

    sidebar.onDeleteRequested(QStringLiteral("entry-test-id"));
    io.drain();

    CHECK(asked == 1);
    CHECK(store->deletes().empty());
}
