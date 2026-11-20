// Every leaf widget against the reference OpenWhisper rendered for it (tools/parity/builders/
// a02.py, tests/reference/<widget>/<theme>-windows11-dpr1-s100.{png,json}): the widget tree
// first, then the pixels, with the application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "app_theme.h"
#include "screenshot/screenshot.h"
#include "settings/testing/memory_store.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/collapsible_header.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/feature_badge.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/update_notice.h"
#include "ui_widgets/wrapped_label.h"

namespace {

const QString kPath =
    QStringLiteral(R"(C:\Users\someone\Documents\Recordings\meeting-2026-11-14.wav)");

// What a builder of tools/parity/builders/a02.py returns: the widget and its pinned focus.
struct Built {
    std::unique_ptr<QWidget> widget;
    QWidget* focus = nullptr;
};

// The settings store a tab container reads its saved tab from: empty, as OpenWhisper's isolated
// settings are, so Quick Record is current.
fv::MemorySettingsStore& tabStore() {
    static fv::MemorySettingsStore store;
    return store;
}

void check(const std::string& widget, const std::string& theme,
           const std::function<Built()>& build) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme));
    Built built = build();
    fv::screenshot::Options options;
    options.focus = built.focus;
    fv::screenshot::expect_matches(*built.widget, widget, theme + "-windows11-dpr1-s100", options);
}

#define REFERENCE_CASE(name, ...)                                                                  \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, "dark", __VA_ARGS__);                                                          \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, "light", __VA_ARGS__);                                                         \
    }

Built tabs(bool recording) {
    auto tabs = std::make_unique<fv::TabbedContentWidget>(tabStore());
    for (int i = 0; i < 4; ++i) {
        tabs->setPage(i, new QWidget);
    }
    tabs->syncStackWithTabBar();
    if (recording) {
        tabs->setRecordingState(true, 0);
    }
    return {std::move(tabs), nullptr};
}

Built updateNotice(bool compact) {
    auto button = std::make_unique<fv::UpdateNoticeButton>();
    if (compact) {
        button->setCompact(true);
    }
    button->showVersion(QStringLiteral("2.9.0"));
    return {std::move(button), nullptr};
}

Built toggle(bool expanded) {
    fv::CollapsibleSectionToggleOptions options;
    options.expanded = expanded;
    return {
        std::make_unique<fv::CollapsibleSectionToggle>(QStringLiteral("Transcription"), options),
        nullptr};
}

} // namespace

REFERENCE_CASE("empty_state", [] {
    auto empty = std::make_unique<fv::EmptyState>(
        QStringLiteral("No snippets yet"),
        QStringLiteral("Type a trigger phrase and the text it expands to."));
    auto* button = new QPushButton(QStringLiteral("Add snippet"));
    empty->addAction(button);
    // The button is the first focusable widget, so it takes focus on show; pinned explicitly.
    return Built{std::move(empty), button};
})

REFERENCE_CASE("button_family", [] {
    auto host = std::make_unique<QWidget>();
    auto* row = new QHBoxLayout(host.get());
    row->addWidget(new fv::Button(QStringLiteral("Cancel")));
    row->addWidget(new fv::PrimaryButton(QStringLiteral("Start")));
    row->addWidget(new fv::DangerButton(QStringLiteral("Delete")));
    row->addWidget(new fv::SuccessButton(QStringLiteral("Save")));
    auto* pause = new fv::WarningButton(QStringLiteral("Pause"));
    row->addWidget(pause);
    pause->setActive(false);
    return Built{std::move(host), nullptr};
})

REFERENCE_CASE("header_card", [] {
    auto card = std::make_unique<fv::HeaderCard>(QStringLiteral("Transcription"), true);
    card->addContentWidget(new QLabel(QStringLiteral("Text")));
    return Built{std::move(card), nullptr};
})

REFERENCE_CASE("settings_switch", [] {
    auto host = std::make_unique<QWidget>();
    auto* row = new QHBoxLayout(host.get());
    row->setContentsMargins(8, 6, 8, 6);
    row->setSpacing(12);
    for (const auto [checked, enabled] : {std::pair{true, true}, {false, true}, {true, false}}) {
        auto* toggle = new fv::SettingsSwitch;
        toggle->setChecked(checked);
        toggle->setEnabled(enabled);
        row->addWidget(toggle);
    }
    row->addStretch();
    return Built{std::move(host), nullptr};
})

REFERENCE_CASE("animated_progress_bar", [] {
    auto bar = std::make_unique<fv::AnimatedProgressBar>();
    bar->setFraction(0.4, false);
    return Built{std::move(bar), nullptr};
})

REFERENCE_CASE("eliding_label",
               [] { return Built{std::make_unique<fv::ElidingLabel>(kPath), nullptr}; })

REFERENCE_CASE("wrapped_label", [] {
    return Built{std::make_unique<fv::WrappedLabel>(kPath, nullptr, true), nullptr};
})

REFERENCE_CASE("feature_badge", [] {
    auto host = std::make_unique<QWidget>();
    auto* row = new QHBoxLayout(host.get());
    row->setContentsMargins(0, 0, 0, 0);
    auto* button = new fv::PrimaryButton(QStringLiteral("Walkthrough"));
    row->addWidget(button);
    auto* badge =
        new fv::FeatureBadge(button, QStringLiteral("Opens a guided tour of this screen."));
    badge->setNew(true);
    return Built{std::move(host), nullptr};
})

REFERENCE_CASE("no_wheel_spin_box", [] {
    auto box = std::make_unique<fv::NoWheelSpinBox>();
    box->setRange(0, 10);
    box->setValue(5);
    return Built{std::move(box), nullptr};
})

REFERENCE_CASE("eliding_combo_box", [] {
    auto combo = std::make_unique<fv::ElidingComboBox>();
    combo->addItems(
        {QStringLiteral("Parakeet TDT 0.6B v3 (multilingual)"), QStringLiteral("Small")});
    return Built{std::move(combo), nullptr};
})

REFERENCE_CASE("update_notice", [] { return updateNotice(false); })
REFERENCE_CASE("update_notice_compact", [] { return updateNotice(true); })
REFERENCE_CASE("tab_bar", [] { return tabs(false); })
REFERENCE_CASE("tab_bar_recording", [] { return tabs(true); })
REFERENCE_CASE("collapsible_toggle", [] { return toggle(true); })
REFERENCE_CASE("collapsible_toggle_collapsed", [] { return toggle(false); })

REFERENCE_CASE("downloads_label", [] {
    return Built{std::make_unique<fv::DownloadsLabel>(
                     QStringLiteral("The model is missing. Get it in Downloads.")),
                 nullptr};
})

REFERENCE_CASE("hotkey_hover_hint", [] {
    // The pill alone; show() starts its fade-in, which a grab does not see.
    auto hint = std::make_unique<fv::HotkeyHoverHint>();
    hint->setHotkey(QStringLiteral("Ctrl+Shift+R"));
    return Built{std::move(hint), nullptr};
})

REFERENCE_CASE("decode_label", [] {
    auto label = std::make_unique<fv::DecodeLabel>();
    // Two spaces in a row are pinned as &nbsp;&nbsp; so the separator keeps its width.
    label->setSegments({{QStringLiteral("Language: "), false},
                        {QStringLiteral("English"), true},
                        {QStringLiteral(u"  \u00b7  "), false},
                        {QStringLiteral("99% match"), true}},
                       false);
    return Built{std::move(label), nullptr};
})
