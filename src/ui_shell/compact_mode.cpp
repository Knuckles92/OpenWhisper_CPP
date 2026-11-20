#include "ui_shell/compact_mode.h"

#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>

#include "appkit/app_context.h"
#include "base/log.h"
#include "pyport/number.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_shell/footer_bar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/page_fit.h"
#include "ui_shell/side_panel_host.h"
#include "ui_shell/title_bar.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/update_notice.h"
#include "uimath/window_fit.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.main_window"};

// int(geo[key]) as _restore_compact_geometry reads it: Python's TypeError and ValueError were
// caught and logged; an int64 beyond the screen's int range is refused the same way.
int compactCoordinate(const Json& geometry, const char* key) {
    const auto value = py::py_int_checked(geometry.at(key));
    if (!value || *value < std::numeric_limits<int>::min() ||
        *value > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(std::string("int() cannot read the compact window's ") + key);
    }
    return static_cast<int>(*value);
}

} // namespace

CompactMode::CompactMode(MainWindow& window) : QObject(&window), window_(window) {}

void CompactMode::setController(QWidget* controller, QVBoxLayout* layout) {
    // The same controller again stays where it is (adding it twice would put it in the layout
    // twice).
    if (controller == controller_) {
        return;
    }
    if (controller_ != nullptr) {
        layout->removeWidget(controller_);
        controller_->hide();
    }
    controller_ = controller;
    if (controller == nullptr) {
        return;
    }
    layout->addWidget(controller);
    controller->setVisible(on_);
}

// main_window.py:1501-1528.
void CompactMode::set(bool compact, bool persist) {
    if (compact == on_) {
        return;
    }
    if (compact && window_.hostMode_) {
        return;
    }
    window_.pageFit().stopAnimation();
    if (compact) {
        enter();
    } else {
        leave();
    }
    if (persist) {
        try {
            window_.context().settings().save_setting(keys::compact_mode.key(), compact);
        } catch (const std::exception& error) {
            kLog.warning("Failed to save compact mode: {}", error.what());
        }
    }
}

// main_window.py:1530-1556.
void CompactMode::enter() {
    TitleBar* titleBar = window_.titleBar();
    if (titleBar->isMaximized()) {
        titleBar->toggleMaximize();
    } else if (window_.isMaximized()) {
        window_.showNormal();
    }

    fullGeometry_ = window_.geometry();
    window_.saveGeometryNow();
    on_ = true;

    window_.tabbedContent()->hide();
    if (controller_ != nullptr) {
        controller_->show();
    }
    // Four footer buttons don't fit the compact width; Stats stays in the tray menu.
    window_.footer()->statsButton()->hide();
    if (QWidget* edgeTab = window_.sidePanelEdgeTab()) {
        edgeTab->hide();
    }
    if (QWidget* panel = window_.sidePanel()) {
        panel->hide();
    }
    titleBar->titleLabel()->hide();
    window_.updateNotice()->setCompact(true);
    titleBar->maximizeButton()->hide();
    window_.setMinimumSize(0, 0);
    window_.setMaximumSize(kUnlimitedHeight, kUnlimitedHeight);
    window_.setFixedSize(window_size::kCompactWidth, window_size::kCompactHeight);
    restoreGeometry();
}

// main_window.py:1558-1582.
void CompactMode::leave() {
    saveGeometry();
    on_ = false;

    window_.setMinimumSize(window_size::kMinWidth, window_.pageFit().floor());
    window_.setMaximumSize(window_size::kMaxWidth + window_.sidePanelHost_->width(),
                           kUnlimitedHeight);
    if (controller_ != nullptr) {
        controller_->hide();
    }
    window_.tabbedContent()->show();
    window_.footer()->statsButton()->show();
    if (QWidget* edgeTab = window_.sidePanelEdgeTab()) {
        edgeTab->show();
    }
    if (QWidget* panel = window_.sidePanel()) {
        panel->show();
    }
    TitleBar* titleBar = window_.titleBar();
    titleBar->titleLabel()->show();
    window_.updateNotice()->setCompact(false);
    titleBar->maximizeButton()->show();

    if (fullGeometry_) {
        window_.setGeometry(*fullGeometry_);
    } else {
        window_.restoreWindowGeometry();
    }
    window_.refitPage();
}

// main_window.py:1584-1589.
void CompactMode::restore() {
    try {
        if (window_.context().settings().get(keys::compact_mode)) {
            set(true, false);
        }
    } catch (const std::exception& error) {
        kLog.warning("Failed to restore compact mode: {}", error.what());
    }
}

// main_window.py:1591-1601.
void CompactMode::saveGeometry() {
    const QRect geometry = window_.geometry();
    Json saved = Json::object();
    saved["x"] = geometry.x();
    saved["y"] = geometry.y();
    try {
        window_.context().settings().save_setting(keys::compact_window_geometry.key(), saved);
    } catch (const std::exception& error) {
        kLog.warning("Failed to save compact window geometry: {}", error.what());
    }
}

// main_window.py:1603-1623.
void CompactMode::restoreGeometry() {
    int x = window_.x();
    int y = window_.y();
    try {
        const Json geometry = window_.context().settings().get(keys::compact_window_geometry);
        if (geometry.is_object() && geometry.contains("x") && geometry.contains("y")) {
            x = compactCoordinate(geometry, "x");
            y = compactCoordinate(geometry, "y");
        }
    } catch (const std::invalid_argument& error) {
        kLog.warning("Invalid compact window geometry: {}", error.what());
    }

    QScreen* screen = QGuiApplication::screenAt(window_.geometry().center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        const QRect available = screen->availableGeometry();
        const Point position = compact_position(
            Point{x, y}, Size{window_.width(), window_.height()},
            Rect{available.x(), available.y(), available.width(), available.height()});
        x = position.x;
        y = position.y;
    }
    window_.move(x, y);
}

} // namespace fv
