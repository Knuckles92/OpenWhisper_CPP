// The restart offer after FioriVoce's program changed under the running app, the restart itself,
// and the previous launch's update error. Ported from tests/test_code_change_restart.py (5 of 6;
// the meeting error's offer waits for M7's meeting runtime), with the offer's box answered by
// clicking its buttons where Python patched QMessageBox.exec.
#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QCheckBox>
#include <QMessageBox>
#include <QWidget>

#include "appcore/activity.h"
#include "backup/restart.h"
#include "controller_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_controller/code_change_restart.h"
#include "ui_controller/quit_confirm.h"
#include "update/apply_error.h"
#include "update/download.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;

namespace {

// Enough of a UI controller for the offer: the activity a restart would stop, the change on disk,
// the quit confirmation's question, and the offers shown, each answered with the next queued
// button label.
struct Controller {
    fv::MemorySettingsStore settings;
    fv::ActivityState activity;
    QWidget window;
    bool changed = false;
    int restarts = 0;
    std::vector<QMessageBox*> shown;
    std::vector<QString> texts;
    std::vector<bool> hadCheckBox;
    std::vector<QString> details;
    std::deque<QString> queued;
    std::function<void(QMessageBox&)> beforeAnswer;
    std::vector<QString> questions;
    bool questionAnswer = false;
    std::unique_ptr<fv::CodeChangeRestart> restart;

    Controller() {
        fv::CodeChangeRestartPorts ports;
        ports.settings = &settings;
        ports.parent = [this] {
            return &window;
        };
        ports.busyWork = [this]() -> std::optional<QString> {
            const auto reasons = fv::busy_reasons(activity.snapshot());
            if (reasons.empty()) {
                return std::nullopt;
            }
            return QString::fromUtf8(reasons.front().data(),
                                     static_cast<qsizetype>(reasons.front().size()));
        };
        ports.changed = [this] {
            return changed;
        };
        ports.execBox = [this](QMessageBox& box) {
            shown.push_back(&box);
            texts.push_back(box.text());
            hadCheckBox.push_back(box.checkBox() != nullptr);
            details.push_back(box.detailedText());
            if (beforeAnswer) {
                beforeAnswer(box);
            }
            REQUIRE_FALSE(queued.empty());
            const QString label = queued.front();
            queued.pop_front();
            for (QAbstractButton* button : box.buttons()) {
                if (button->text() == label) {
                    button->click();
                    return;
                }
            }
            FAIL("no button " << label.toStdString());
        };
        ports.confirmQuit = [this] {
            return fv::confirmQuitWhileBusy(&window, activity.snapshot(),
                                            [this](QWidget*, const fv::QuitQuestion& question) {
                                                questions.push_back(question.text);
                                                return questionAnswer;
                                            });
        };
        ports.restart = [this] {
            ++restarts;
        };
        restart = std::make_unique<fv::CodeChangeRestart>(std::move(ports));
        // UIController's constructor starts it.
        restart->start();
    }
};

} // namespace

// from
// tests/test_code_change_restart.py::test_the_poll_asks_once_the_code_changed_and_nothing_is_running
TEST_CASE("the poll asks once the code changed and nothing is running",
          "[test_code_change_restart][widget]") {
    Controller controller;

    controller.restart->poll();
    CHECK(controller.texts.empty());

    controller.activity.set_recording(true);
    controller.changed = true;
    controller.restart->poll();
    CHECK(controller.texts.empty());
    CHECK(controller.restart->polling());

    controller.activity.set_recording(false);
    controller.queued.push_back(QStringLiteral("Later"));
    controller.restart->poll();

    REQUIRE(controller.texts.size() == 1);
    CHECK(controller.texts[0].contains(QStringLiteral("updated while it was running")));
    CHECK(controller.restarts == 0);
    // Later means not again.
    CHECK_FALSE(controller.restart->polling());
}

// from tests/test_code_change_restart.py::test_dont_show_again_mutes_the_unprompted_offer
TEST_CASE("dont show again mutes the unprompted offer", "[test_code_change_restart][widget]") {
    Controller controller;
    controller.beforeAnswer = [](QMessageBox& box) {
        box.checkBox()->setChecked(true);
    };
    controller.queued.push_back(QStringLiteral("Later"));
    controller.restart->offer();
    CHECK(controller.settings
              .contents()[std::string(fv::keys::update_restart_prompt_enabled.key())] ==
          fv::Json(false));

    controller.changed = true;
    controller.restart->start();
    controller.restart->poll();
    CHECK(controller.texts.size() == 1);
    CHECK_FALSE(controller.restart->polling());
}

// from tests/test_code_change_restart.py::test_an_error_offer_has_no_dont_show_again
TEST_CASE("an error offer has no dont show again", "[test_code_change_restart][widget]") {
    Controller controller;
    controller.queued.push_back(QStringLiteral("Later"));
    controller.restart->offer(QStringLiteral("boom"));
    REQUIRE(controller.hadCheckBox.size() == 1);
    CHECK_FALSE(controller.hadCheckBox[0]);
    CHECK(controller.details[0] == QStringLiteral("boom"));
    CHECK(controller.texts[0] ==
          QStringLiteral("FioriVoce was updated while it was running, which likely caused this "
                         "error."));
}

// from tests/test_code_change_restart.py::test_restart_now_confirms_running_work_before_restarting
TEST_CASE("restart now confirms running work before restarting",
          "[test_code_change_restart][widget]") {
    Controller controller;
    controller.activity.set_meeting_active(true);
    controller.questionAnswer = false;
    controller.queued.push_back(QStringLiteral("Restart now"));
    controller.restart->offer();
    REQUIRE(controller.questions.size() == 1);
    CHECK(controller.questions[0].contains(QStringLiteral("a meeting")));
    CHECK(controller.restarts == 0);

    controller.activity.set_meeting_active(false);
    controller.queued.push_back(QStringLiteral("Restart now"));
    controller.restart->offer();
    CHECK(controller.restarts == 1);
}

// from tests/test_code_change_restart.py::test_a_restart_exits_with_the_code_main_relaunches_on
// (MainWindow's force-quit flag is private and QCoreApplication::exit closes no window, so only the
// geometry save and the exit code are observed)
TEST_CASE("a restart exits with the code main relaunches on", "[test_code_change_restart]") {
    int saves = 0;
    std::vector<int> exits;
    fv::restartApp([&saves] { ++saves; }, [&exits](int code) { exits.push_back(code); });
    CHECK(exits == std::vector<int>{fv::backup::restart_exit_code});
    CHECK(exits[0] == 73);
    CHECK(saves == 1);
}

TEST_CASE("the controller polls for a changed program and restarts on Restart now", "[widget]") {
    RigOptions options;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.codeChanged = [] {
            return true;
        };
        platform.execRestartOffer = [](QMessageBox& box) {
            for (QAbstractButton* button : box.buttons()) {
                if (button->text() == QStringLiteral("Restart now")) {
                    button->click();
                }
            }
        };
    };
    ControllerRig rig(options);
    fv::CodeChangeRestart* restart = rig.feature().codeChangeRestart();
    REQUIRE(restart != nullptr);
    CHECK(restart->polling());
    restart->poll();
    CHECK(rig.calls().has("exit:73"));
    CHECK_FALSE(restart->polling());
}

// show_apply_error_if_any (ui_controller.py:2832-2837).
TEST_CASE("a leftover update error is shown once in an Update failed box", "[widget]") {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path updates = root.path() / "updates";
    fv::update::write_apply_error(updates, "The setup could not replace FioriVoce.");
    std::vector<std::string> boxes;
    const auto warning = [&boxes](QWidget*, const QString& title, const QString& text) {
        boxes.push_back(title.toStdString() + ":" + text.toStdString());
    };
    CHECK(fv::showApplyErrorIfAny(updates, nullptr, warning));
    CHECK(boxes ==
          std::vector<std::string>{"Update failed:The setup could not replace FioriVoce."});
    CHECK_FALSE(fv::showApplyErrorIfAny(updates, nullptr, warning));
    CHECK(boxes.size() == 1);
}

TEST_CASE("the controller shows the previous launch's update error once the window shows",
          "[widget]") {
    ControllerRig rig;
    fv::update::write_apply_error(fv::update::updates_root(rig.paths()), "The update failed.");
    rig.mainWindowShown();
    CHECK(rig.calls().has("warning:Update failed:The update failed."));
    CHECK_FALSE(std::filesystem::exists(
        fv::update::apply_error_path(fv::update::updates_root(rig.paths()))));
}
