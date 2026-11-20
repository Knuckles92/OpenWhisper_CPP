#include "agents/testing/fake_installed_agents.h"
#include "history_ui_support.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/manual_cleanup_dialog.h"
#include "ui_widgets/buttons.h"
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("manual cleanup requires a named agent and an explicit send", "[widget]") {
    fvtest::Io io;
    int calls = 0;
    std::string destination;
    std::string material;
    fv::ManualCleanupDialog dialog(QStringLiteral("Private words"), io.executor(),
                                   [&](std::string agent, std::string text, fv::CancelToken) {
                                       ++calls;
                                       destination = std::move(agent);
                                       material = std::move(text);
                                       return fv::agents::PassOutcome{.ok = true,
                                                                      .text = "Clean words"};
                                   });
    CHECK_FALSE(dialog.sendButton()->isEnabled());
    dialog.agentChoice()->setCurrentIndex(1);
    CHECK(dialog.sendButton()->isEnabled());
    CHECK(calls == 0);
    dialog.sendButton()->click();
    io.drain();
    CHECK(calls == 1);
    CHECK_FALSE(destination.empty());
    CHECK(material == "Private words");
    CHECK(dialog.resultText()->toPlainText() == QStringLiteral("Clean words"));
    QString copied;
    dialog.copyText = [&](const QString& text) {
        copied = text;
        return true;
    };
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Copy result"))
            button->click();
    }
    CHECK(copied == QStringLiteral("Clean words"));
}

TEST_CASE("closing manual cleanup cancels work before a fake sends", "[widget]") {
    fvtest::Io io;
    int sends = 0;
    {
        fv::ManualCleanupDialog dialog(QStringLiteral("Private words"), io.executor(),
                                       [&](std::string, std::string, fv::CancelToken token) {
                                           if (!token.cancelled())
                                               ++sends;
                                           return fv::agents::PassOutcome{};
                                       });
        dialog.agentChoice()->setCurrentIndex(1);
        dialog.sendButton()->click();
    }
    io.drain();
    CHECK(sends == 0);
}

TEST_CASE("history opens cleanup for the chosen displayed transcript", "[widget]") {
    fvtest::Io io;
    auto store = std::make_shared<fvtest::FakeStore>();
    fv::HistoryEntry entry;
    entry.id = "example";
    entry.text = "Selected transcript";
    fv::HistoryEntryDialog history(entry, store, io.executor(), nullptr, true);
    CHECK_FALSE(history.agentCleanupButton()->isHidden());
    fv::HistoryEntryDialog unavailable(entry, store, io.executor(), nullptr, false);
    CHECK(unavailable.agentCleanupButton() == nullptr);
    bool opened = false;
    history.runCleanupDialog = [&](fv::ManualCleanupDialog& dialog) {
        opened = true;
        CHECK_FALSE(dialog.sendButton()->isEnabled());
    };
    history.agentCleanupButton()->click();
    CHECK(opened);
}

TEST_CASE("manual cleanup can retry after an agent failure", "[widget]") {
    fvtest::Io io;
    int calls = 0;
    fv::ManualCleanupDialog dialog(QStringLiteral("Words"), io.executor(),
                                   [&](std::string, std::string, fv::CancelToken) {
                                       ++calls;
                                       if (calls == 1)
                                           return fv::agents::PassOutcome{.error = "Unavailable"};
                                       return fv::agents::PassOutcome{.ok = true, .text = "Words."};
                                   });
    dialog.agentChoice()->setCurrentIndex(1);
    CHECK_FALSE(dialog.sendButton()->autoDefault());
    dialog.sendButton()->click();
    io.drain();
    REQUIRE(dialog.sendButton()->isEnabled());
    dialog.sendButton()->click();
    io.drain();
    CHECK(calls == 2);
    CHECK(dialog.resultText()->toPlainText() == QStringLiteral("Words."));
}

TEST_CASE("manual cleanup can start again after closing a canceled request", "[widget]") {
    fvtest::Io io;
    int sends = 0;
    auto runner = [&](std::string, std::string, fv::CancelToken token) {
        if (!token.cancelled())
            ++sends;
        return fv::agents::PassOutcome{.ok = !token.cancelled(), .text = "Ready"};
    };
    {
        fv::ManualCleanupDialog first(QStringLiteral("Words"), io.executor(), runner);
        first.agentChoice()->setCurrentIndex(1);
        first.sendButton()->click();
    }
    fv::ManualCleanupDialog second(QStringLiteral("Words"), io.executor(), runner);
    second.agentChoice()->setCurrentIndex(1);
    second.sendButton()->click();
    io.drain();
    CHECK(sends == 1);
    CHECK(second.resultText()->toPlainText() == QStringLiteral("Ready"));
}

TEST_CASE("history discovers the installed-agent service without starting a probe", "[widget]") {
    const auto previous = fv::agents::installed_agents_if_set();
    struct Restore {
        std::shared_ptr<fv::agents::InstalledAgents> previous;
        ~Restore() { fv::agents::set_installed_agents(std::move(previous)); }
    } restore{previous};
    auto fake = std::make_shared<fv::agents::FakeInstalledAgents>();
    fv::agents::set_installed_agents(fake);
    fvtest::Io io;
    auto store = std::make_shared<fvtest::FakeStore>();
    fv::HistoryEntry entry;
    entry.id = "example";
    entry.text = "Selected transcript";
    fv::HistoryEntryDialog history(entry, store, io.executor());
    REQUIRE(history.agentCleanupButton() != nullptr);
    CHECK(fake->lookups() == 0);
}
