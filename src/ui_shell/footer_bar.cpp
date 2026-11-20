#include "ui_shell/footer_bar.h"

#include <QHBoxLayout>
#include <QPushButton>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

const QString kFooterBarStyle = QStringLiteral(R"(
        QWidget#footerBar {
            background-color: @bg;
            border-top: 1px solid @border-subtle;
        }
    )");

const QString kTrayButtonStyle = QStringLiteral(R"(
        QPushButton#trayButton {
            background-color: @surface;
            color: @text-body;
            border: 1px solid @border;
            border-radius: 8px;
            padding: 6px 18px;
            font-weight: 600;
            font-size: 13px;
        }
        QPushButton#trayButton:hover {
            background-color: @accent;
            color: @on-accent;
            border: 1px solid @accent;
        }
        QPushButton#trayButton:pressed {
            background-color: @accent-pressed;
            color: @on-accent;
        }
    )");

const QString kStatsButtonStyle = QStringLiteral(R"(
        QPushButton#statsButton {
            background-color: @surface;
            color: @purple-text;
            border: 1px solid @border;
            border-radius: 8px;
            padding: 6px 18px;
            font-weight: 600;
            font-size: 13px;
        }
        QPushButton#statsButton:hover {
            background-color: @purple;
            color: @on-accent;
            border: 1px solid @purple;
        }
        QPushButton#statsButton:pressed {
            background-color: rgba(@purple-rgb, 0.8);
            color: @on-accent;
        }
    )");

const QString kSettingsButtonStyle = QStringLiteral(R"(
        QPushButton#settingsButton {
            background-color: @surface;
            color: @accent-cyan;
            border: 1px solid @border;
            border-radius: 8px;
            padding: 6px 18px;
            font-weight: 600;
            font-size: 13px;
        }
        QPushButton#settingsButton:hover {
            background-color: @accent;
            color: @on-accent;
            border: 1px solid @accent;
        }
        QPushButton#settingsButton:pressed {
            background-color: @accent-pressed;
            color: @on-accent;
        }
    )");

const QString kQuitButtonStyle = QStringLiteral(R"(
        QPushButton#quitButton {
            background-color: @surface;
            color: @danger-text;
            border: 1px solid @border;
            border-radius: 8px;
            padding: 6px 18px;
            font-weight: 600;
            font-size: 13px;
        }
        QPushButton#quitButton:hover {
            background-color: @danger;
            color: @on-accent;
            border: 1px solid @danger;
        }
        QPushButton#quitButton:pressed {
            background-color: @danger-pressed;
            color: @on-accent;
        }
    )");

QPushButton* footerButton(const QString& text, const QString& name, int minimumWidth,
                          const QString& style) {
    auto* button = new QPushButton(text);
    button->setObjectName(name);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(FooterBar::kButtonHeight);
    button->setMinimumWidth(minimumWidth);
    ui::setLocalStyleSheet(button, style);
    return button;
}

} // namespace

// main_window.py:893-957 _build_footer (the classic window: "Minimize to Tray", not the
// compositor-managed "Hide").
FooterBar::FooterBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("footerBar"));
    setFixedHeight(kHeight);
    ui::setLocalStyleSheet(this, kFooterBarStyle);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 7, 16, 7);
    layout->setSpacing(0);
    layout->addStretch();

    trayButton_ = new Button(QStringLiteral("Minimize to Tray"));
    trayButton_->setObjectName(QStringLiteral("trayButton"));
    trayButton_->setCursor(Qt::PointingHandCursor);
    trayButton_->setFixedHeight(kButtonHeight);
    trayButton_->setMinimumWidth(140);
    trayButton_->setToolTip(
        QStringLiteral("Hide %1 in the system tray").arg(qt::toQString(identity().product)));
    ui::setLocalStyleSheet(trayButton_, kTrayButtonStyle);
    layout->addWidget(trayButton_);

    layout->addSpacing(kGap);

    statsButton_ =
        footerButton(QStringLiteral("Stats"), QStringLiteral("statsButton"), 80, kStatsButtonStyle);
    statsButton_->setToolTip(QStringLiteral("Your words, pace, streak and favorite apps"));
    layout->addWidget(statsButton_);

    layout->addSpacing(kGap);

    settingsButton_ = footerButton(QStringLiteral("Settings"), QStringLiteral("settingsButton"),
                                   100, kSettingsButtonStyle);
    settingsButton_->setToolTip(
        QStringLiteral("Settings, models, and downloads (starts on the Overview)"));
    layout->addWidget(settingsButton_);

    layout->addSpacing(kGap);

    quitButton_ =
        footerButton(QStringLiteral("Quit"), QStringLiteral("quitButton"), 100, kQuitButtonStyle);
    quitHint_ = new HotkeyHintFilter(quitButton_, QString::fromLatin1(kQuitShortcut));
    layout->addWidget(quitButton_);

    layout->addStretch();
}

void FooterBar::setTrayAvailable(bool available) {
    trayButton_->setVisible(available);
    if (!available) {
        trayButton_->setEnabled(false);
        trayButton_->setToolTip(
            QStringLiteral("This desktop session does not provide a system tray."));
    }
}

void FooterBar::setTrayHotkey(const QString& hotkey) {
    trayButton_->setHotkey(hotkey);
}

} // namespace fv
