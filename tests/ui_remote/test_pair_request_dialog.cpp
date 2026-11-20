// The pairing question (test_remote_pairing_ux.py's prompter and dialog functions):
// PairRequestDialog and the prompter that shows it, over FakeRemoteEngine with ManualExecutors.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QLabel>
#include <QPointer>
#include <QWidget>

#include "base/testing/manual_executor.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/pair_request_dialog.h"
#include "ui_remote/pair_request_prompter.h"
#include "ui_remote/remote_text.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::remote::testing::FakeRemoteEngine;

struct Prompting {
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    FakeRemoteEngine engine;
    std::unique_ptr<QWidget> window = std::make_unique<QWidget>();

    Prompting() { fv::installPairRequestPrompter(engine, io, ui, *window); }

    void ask(const char* id, double seconds = 90) {
        engine.state.pair_request =
            fv::remote::PairRequestInfo{id, "laptop", "192.168.1.30", "042917", seconds};
        notify();
    }

    void notify(const std::string& kind = "pair_request") {
        engine.emit_changed(kind);
        pump();
    }

    void pump() {
        for (int round = 0; round < 10; ++round) {
            const auto ran = ui.run_all() + io.run_all();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (ran == 0) {
                return;
            }
        }
    }

    fv::PairRequestDialog* dialog() const {
        const auto dialogs = window->findChildren<fv::PairRequestDialog*>();
        const auto open = std::ranges::find_if(dialogs, [](auto* d) { return d->isVisible(); });
        return open != dialogs.end() ? *open : nullptr;
    }

    std::vector<std::string> answers() const {
        std::vector<std::string> found;
        for (const auto& call : engine.calls()) {
            if (call.starts_with("answer_pair_request")) {
                found.push_back(call);
            }
        }
        return found;
    }
};

} // namespace

TEST_CASE("test_the_host_asks_with_the_number_and_passes_the_answer_on", "[widget]") {
    Prompting prompting;
    prompting.ask("r1");
    auto* dialog = prompting.dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->objectName() == QStringLiteral("pairRequestDialog"));
    CHECK(dialog->windowTitle() == QStringLiteral("Pair a computer"));
    CHECK_FALSE(dialog->isModal());
    CHECK(dialog->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    CHECK(dialog->minimumWidth() == 440);
    CHECK(dialog->codeLabel()->text() == QStringLiteral("042 917"));
    CHECK(dialog->codeLabel()->accessibleName() == QStringLiteral("Pairing number"));
    const auto expiry = dialog->expiryLabel()->text();
    CHECK((expiry == QStringLiteral("No answer by 1:29 turns it down.") ||
           expiry == QStringLiteral("No answer by 1:30 turns it down.")));
    const auto labels = dialog->findChildren<fv::WrappedLabel*>();
    QStringList texts;
    for (auto* label : labels) {
        texts.append(label->text());
    }
    CHECK(texts.contains(QStringLiteral("laptop wants to use this computer's engine")));
    CHECK(texts.contains(QStringLiteral(
        "It's asking from 192.168.1.30. Allow it only if laptop shows this same number:")));
    CHECK(dialog->denyButton()->isDefault());
    CHECK(dialog->allowButton()->text() == QStringLiteral("Allow"));
    dialog->allowButton()->click();
    prompting.pump();
    CHECK(prompting.answers() == std::vector<std::string>{"answer_pair_request(r1,true)"});
    CHECK(prompting.dialog() == nullptr);
}

TEST_CASE("PairRequestDialog says this network and A computer without an address or name",
          "[widget]") {
    QWidget window;
    fv::PairRequestDialog dialog({"r9", "", "", "123456", 30}, &window);
    QStringList texts;
    for (auto* label : dialog.findChildren<fv::WrappedLabel*>()) {
        texts.append(label->text());
    }
    CHECK(texts.contains(QStringLiteral("A computer wants to use this computer's engine")));
    CHECK(texts.contains(QStringLiteral(
        "It's asking from this network. Allow it only if A computer shows this same number:")));
    QFont plain = QWidget().font();
    CHECK(dialog.codeLabel()->font().bold());
    CHECK(dialog.codeLabel()->font().pointSizeF() == plain.pointSizeF() * 2.4);
    CHECK(dialog.codeLabel()->font().letterSpacing() == 4);
    CHECK(dialog.codeLabel()->alignment() == Qt::AlignCenter);
}

TEST_CASE("test_the_question_says_what_else_allowing_grants", "[widget]") {
    Prompting prompting;
    prompting.ask("r1");
    REQUIRE(prompting.dialog() != nullptr);
    CHECK(prompting.dialog()->grantsLabel()->text() ==
          QStringLiteral("Once allowed, it can dictate and transcribe with the engine selected "
                         "here. You can remove it any time under Settings → Remote engine."));
    prompting.dialog()->dismiss();
    prompting.pump();

    prompting.engine.grants = {"keep its dictations and meetings here", "manage its models"};
    prompting.ask("r2");
    REQUIRE(prompting.dialog() != nullptr);
    CHECK(prompting.dialog()->grantsLabel()->text().startsWith(
        QStringLiteral("Once allowed, it can dictate and transcribe with the engine selected "
                       "here. It can also keep its dictations and meetings here and manage its "
                       "models.")));
    CHECK(prompting.dialog()->grantsLabel()->text() ==
          QString::fromStdString(fv::remote_text::pairing_grants_phrase(prompting.engine.grants)));
}

TEST_CASE("test_closing_the_question_denies_and_a_withdrawn_one_just_goes", "[widget]") {
    Prompting prompting;
    prompting.ask("r1");
    REQUIRE(prompting.dialog() != nullptr);
    prompting.dialog()->close();
    prompting.pump();
    CHECK(prompting.answers() == std::vector<std::string>{"answer_pair_request(r1,false)"});

    prompting.ask("r2");
    QPointer<fv::PairRequestDialog> second = prompting.dialog();
    REQUIRE(second);
    CHECK(second->requestId() == "r2");
    prompting.engine.state.pair_request.reset();
    prompting.notify();
    CHECK((second.isNull() || !second->isVisible()));
    CHECK(prompting.dialog() == nullptr);
    CHECK(prompting.answers().size() == 1);

    // A replaced request's dialog goes too, and the new one is asked.
    prompting.ask("r3");
    QPointer<fv::PairRequestDialog> third = prompting.dialog();
    REQUIRE(third);
    prompting.ask("r4");
    CHECK((third.isNull() || !third->isVisible()));
    REQUIRE(prompting.dialog() != nullptr);
    CHECK(prompting.dialog()->requestId() == "r4");
    CHECK(prompting.answers().size() == 1);
}

TEST_CASE("Escape on the pairing question denies it", "[widget]") {
    Prompting prompting;
    prompting.ask("r1");
    REQUIRE(prompting.dialog() != nullptr);
    prompting.dialog()->reject();
    prompting.pump();
    CHECK(prompting.answers() == std::vector<std::string>{"answer_pair_request(r1,false)"});
}

TEST_CASE("a request that has run out is closed without an answer", "[widget]") {
    QWidget window;
    std::vector<bool> answered;
    auto* dialog = new fv::PairRequestDialog({"r1", "laptop", "", "123456", 0}, &window);
    QObject::connect(dialog, &fv::PairRequestDialog::answered,
                     [&](bool allow) { answered.push_back(allow); });
    CHECK(dialog->expiryLabel()->text() == QStringLiteral("No answer by 0:00 turns it down."));
    dialog->show();
    dialog->close();
    CHECK(answered.empty());
}

TEST_CASE("test_a_replaced_prompter_is_not_held_by_a_server_thread", "[widget]") {
    Prompting prompting;
    prompting.engine.state.pair_request =
        fv::remote::PairRequestInfo{"r1", "laptop", "192.168.1.30", "042917", 90};
    prompting.engine.emit_changed("pair_request");
    prompting.ui.run_all(); // a read is now waiting on io
    prompting.window.reset();
    prompting.engine.clear_calls();
    std::thread server([&] { prompting.engine.emit_changed("pair_request"); });
    server.join();
    prompting.pump();
    CHECK(prompting.engine.calls().size() <= 2); // the read already queued may still run
    CHECK(prompting.answers().empty());
    prompting.engine.emit_changed("pair_request");
    prompting.pump();
    CHECK(prompting.answers().empty());
}
