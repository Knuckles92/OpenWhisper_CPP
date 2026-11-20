// The first-visit Meeting Mode overview: copy, dismiss, and when it appears (OpenWhisper's
// tests/test_meeting_intro_dialog.py). The five tests that open the main window on the Meeting
// Mode tab are M7.20's.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include <QLabel>
#include <QPushButton>
#include <QStringList>

#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_meeting/meeting_intro_dialog.h"

using fv::MeetingModeIntroDialog;

namespace {

QString dialogText(const MeetingModeIntroDialog& dialog) {
    QStringList texts;
    for (const QLabel* label : dialog.findChildren<QLabel*>()) {
        if (!label->text().isEmpty()) {
            texts << label->text();
        }
    }
    return texts.join(QLatin1Char(' '));
}

QPushButton* button(MeetingModeIntroDialog& dialog, const char* name) {
    return dialog.findChild<QPushButton*>(QString::fromLatin1(name));
}

} // namespace

// from
// tests/test_meeting_intro_dialog.py::TestMeetingIntroDialog::test_copy_covers_mode_features_and_settings
TEST_CASE("test_copy_covers_mode_features_and_settings", "[test_meeting_intro_dialog][widget]") {
    MeetingModeIntroDialog dialog;
    const QString text = dialogText(dialog);
    CHECK(text.contains(QStringLiteral("Welcome to Meeting Mode")));
    CHECK(text.toLower().contains(QStringLiteral("live transcript")));
    CHECK(text.toLower().contains(QStringLiteral("dashboard")));
    CHECK(text.toLower().contains(QStringLiteral("ai insights")));
    CHECK(text.contains(QStringLiteral("Settings → Meeting Mode")));
    // Meeting models live on Settings pages now, not a separate window.
    CHECK_FALSE(text.contains(QStringLiteral("Model Manager")));
    CHECK(text.contains(QStringLiteral("Skip anytime")));

    QPushButton* skip = button(dialog, "meetingIntroSkipButton");
    QPushButton* gotIt = button(dialog, "meetingIntroGotItButton");
    REQUIRE(skip != nullptr);
    REQUIRE(gotIt != nullptr);
    CHECK(skip->isEnabled());
    CHECK(gotIt->isEnabled());
    CHECK(gotIt->isDefault());
}

// from tests/test_meeting_intro_dialog.py::TestMeetingIntroDialog::test_skip_keeps_skip_result
TEST_CASE("test_skip_keeps_skip_result", "[test_meeting_intro_dialog][widget]") {
    MeetingModeIntroDialog dialog;
    button(dialog, "meetingIntroSkipButton")->click();
    CHECK(dialog.resultAction() == MeetingModeIntroDialog::kResultSkip);
}

// from tests/test_meeting_intro_dialog.py::TestMeetingIntroDialog::test_got_it_records_result
TEST_CASE("test_got_it_records_result", "[test_meeting_intro_dialog][widget]") {
    MeetingModeIntroDialog dialog;
    button(dialog, "meetingIntroGotItButton")->click();
    CHECK(dialog.resultAction() == MeetingModeIntroDialog::kResultGotIt);
}

// from
// tests/test_meeting_intro_dialog.py::TestMaybeShowMeetingIntro::test_skips_dialog_after_it_has_been_seen
TEST_CASE("test_skips_dialog_after_it_has_been_seen", "[test_meeting_intro_dialog][widget]") {
    fv::MemorySettingsStore settings(fv::Json{{"meeting_mode_intro_seen", true}});
    bool shown = false;

    const bool result = fv::maybeShowMeetingModeIntro(
        settings, nullptr, [&shown](MeetingModeIntroDialog&) { shown = true; });

    CHECK_FALSE(result);
    CHECK_FALSE(shown);
}

// from
// tests/test_meeting_intro_dialog.py::TestMaybeShowMeetingIntro::test_skip_and_got_it_both_persist_seen
TEST_CASE("test_skip_and_got_it_both_persist_seen", "[test_meeting_intro_dialog][widget]") {
    const std::string action = GENERATE(std::string("skip"), std::string("got_it"));
    CAPTURE(action);
    fv::MemorySettingsStore settings(fv::Json{{"meeting_mode_intro_seen", false}});
    QString left;

    const bool result =
        fv::maybeShowMeetingModeIntro(settings, nullptr, [&](MeetingModeIntroDialog& dialog) {
            button(dialog, action == "skip" ? "meetingIntroSkipButton" : "meetingIntroGotItButton")
                ->click();
            left = dialog.resultAction();
        });

    CHECK(result);
    CHECK(left == QString::fromStdString(action));
    CHECK(settings.get(fv::keys::meeting_mode_intro_seen));
}

TEST_CASE("a dialog closed without a button still counts as seen",
          "[test_meeting_intro_dialog][widget]") {
    fv::MemorySettingsStore settings;

    const bool result = fv::maybeShowMeetingModeIntro(
        settings, nullptr, [](MeetingModeIntroDialog& dialog) { dialog.reject(); });

    CHECK(result);
    CHECK(settings.get(fv::keys::meeting_mode_intro_seen));
}

TEST_CASE("a settings file that cannot be written is only logged",
          "[test_meeting_intro_dialog][widget]") {
    fv::MemorySettingsStore settings;
    settings.file().fail_next_replace("disk full");

    const bool result = fv::maybeShowMeetingModeIntro(
        settings, nullptr, [](MeetingModeIntroDialog& dialog) { dialog.accept(); });

    CHECK(result);
    CHECK_FALSE(settings.get(fv::keys::meeting_mode_intro_seen));
}
