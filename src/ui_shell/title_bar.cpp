#include "ui_shell/title_bar.h"

#include <algorithm>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"

namespace fv {
namespace {

// The glyph's box, in pixels.
constexpr int kGlyphSize = 10;

const QString kMenuBarStyle = QStringLiteral(R"(
        QMenuBar {
            background-color: transparent;
            color: @text-secondary;
            font-size: 12px;
            border: none;
            spacing: 0px;
        }
        QMenuBar::item {
            background-color: transparent;
            padding: 4px 10px;
        }
        QMenuBar::item:selected {
            background-color: @surface-hover;
            color: @text-heading;
        }
        QMenuBar::item:pressed {
            background-color: @surface-active;
        }
        QMenu::separator {
            height: 1px;
            background-color: @surface-hover;
            margin: 4px 8px;
        }
    )");

const QString kTitleLabelStyle = QStringLiteral(R"(
        QLabel {
            background-color: transparent;
            color: @text;
            font-size: 13px;
            font-weight: 600;
            font-family: 'Segoe UI', sans-serif;
        }
    )");

const QString kWindowButtonStyle = QStringLiteral(R"(
        QPushButton {
            background-color: transparent;
            border: none;
            color: @text-secondary;
            font-size: 14px;
            font-family: 'Segoe UI', sans-serif;
        }
        QPushButton:hover {
            background-color: @surface-hover;
            color: @text-heading;
        }
    )");

const QString kCloseButtonStyle = QStringLiteral(R"(
        QPushButton {
            background-color: transparent;
            border: none;
            color: @text-secondary;
            font-size: 14px;
            font-family: 'Segoe UI', sans-serif;
        }
        QPushButton:hover {
            background-color: @danger;
            color: @on-accent;
        }
    )");

const QString kTitleBarStyle = QStringLiteral(R"(
        #customTitleBar {
            background-color: @surface;
            border-bottom: 1px solid @border;
        }
    )");

} // namespace

WindowGlyphButton::WindowGlyphButton(Glyph glyph, const char* hoverToken, QWidget* parent)
    : QPushButton(parent), glyph_(glyph), hoverToken_(hoverToken) {
    setAttribute(Qt::WA_Hover, true);
}

QString WindowGlyphButton::glyphName() const {
    switch (glyph_) {
    case Glyph::Minimize:
        return QStringLiteral("minimize");
    case Glyph::Maximize:
        return QStringLiteral("maximize");
    case Glyph::Restore:
        return QStringLiteral("restore");
    case Glyph::Close:
        return QStringLiteral("close");
    }
    return {};
}

void WindowGlyphButton::setGlyph(Glyph glyph) {
    glyph_ = glyph;
    update();
}

void WindowGlyphButton::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event); // the stylesheet's hover background
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = ui::tokenColor(underMouse() ? hoverToken_ : "text-secondary");
    QPen pen(color, 1.0);
    pen.setCapStyle(Qt::FlatCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    constexpr int size = kGlyphSize;
    // A whole-pixel origin plus half a pixel keeps 1 px lines crisp. Python's // floors, and
    // the button is never narrower than the glyph, so integer division agrees.
    const double left = (width() - size) / 2 + 0.5;
    const double top = (height() - size) / 2 + 0.5;
    const double right = left + size - 1;
    const double bottom = top + size - 1;
    switch (glyph_) {
    case Glyph::Minimize: {
        const double y = top + (size - 1) / 2;
        painter.drawLine(QPointF(left, y), QPointF(right, y));
        break;
    }
    case Glyph::Maximize:
        painter.drawRect(QRectF(left, top, size - 1, size - 1));
        break;
    case Glyph::Restore:
        // The front window, and the edges of the one behind that show above and to its right.
        painter.drawRect(QRectF(left, top + 2, size - 3, size - 3));
        painter.drawLine(QPointF(left + 2, top), QPointF(right, top));
        painter.drawLine(QPointF(right, top), QPointF(right, bottom - 2));
        break;
    case Glyph::Close:
        painter.drawLine(QPointF(left, top), QPointF(right, bottom));
        painter.drawLine(QPointF(right, top), QPointF(left, bottom));
        break;
    }
    painter.end();
}

TitleBar::TitleBar(QWidget* window, FramelessWindowHost* host)
    : QFrame(window), window_(window), host_(host) {
    setFixedHeight(kBaseHeight);
    setObjectName(QStringLiteral("customTitleBar"));
    setAutoFillBackground(true);
    // Before any child exists: Qt restyles a widget's children before it sends the widget its
    // own StyleChange, so children styled first parsed the raw "@surface" sheet and logged
    // "Could not parse stylesheet of object CustomTitleBar" on every launch.
    ui::setLocalStyleSheet(this, kTitleBarStyle);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 0, 0, 0);
    layout_->setSpacing(0);

    menuBar_ = new QMenuBar;
    ui::setLocalStyleSheet(menuBar_, kMenuBarStyle);
    layout_->addWidget(menuBar_, 0, Qt::AlignVCenter);
    layout_->addStretch();

    titleLabel_ = new QLabel(qt::toQString(identity().product));
    ui::setLocalStyleSheet(titleLabel_, kTitleLabelStyle);
    layout_->addWidget(titleLabel_);
    layout_->addStretch();

    minimizeButton_ = new WindowGlyphButton(WindowGlyphButton::Glyph::Minimize);
    minimizeButton_->setFixedSize(kButtonWidth, kBaseHeight);
    ui::setLocalStyleSheet(minimizeButton_, kWindowButtonStyle);
    minimizeButton_->setToolTip(QStringLiteral("Minimize"));
    minimizeButton_->setAccessibleName(QStringLiteral("Minimize window"));
    connect(minimizeButton_, &QPushButton::clicked, this, [this] {
        if (window_ != nullptr) {
            window_->showMinimized();
        }
    });

    maximizeButton_ = new WindowGlyphButton(WindowGlyphButton::Glyph::Maximize);
    maximizeButton_->setFixedSize(kButtonWidth, kBaseHeight);
    ui::setLocalStyleSheet(maximizeButton_, kWindowButtonStyle);
    maximizeButton_->setToolTip(QStringLiteral("Maximize"));
    maximizeButton_->setAccessibleName(QStringLiteral("Maximize window"));
    connect(maximizeButton_, &QPushButton::clicked, this, &TitleBar::toggleMaximize);

    closeButton_ = new WindowGlyphButton(WindowGlyphButton::Glyph::Close, "on-accent");
    closeButton_->setFixedSize(kButtonWidth, kBaseHeight);
    ui::setLocalStyleSheet(closeButton_, kCloseButtonStyle);
    closeButton_->setToolTip(QStringLiteral("Close"));
    closeButton_->setAccessibleName(QStringLiteral("Close window"));
    connect(closeButton_, &QPushButton::clicked, this, [this] {
        if (window_ != nullptr) {
            window_->close();
        }
    });

    layout_->addWidget(minimizeButton_);
    layout_->addWidget(maximizeButton_);
    layout_->addWidget(closeButton_);

    syncHeight();
}

void TitleBar::addNotice(QWidget* widget) {
    layout_->insertWidget(layout_->indexOf(minimizeButton_), widget, 0, Qt::AlignVCenter);
}

void TitleBar::syncHeight() {
    const int border = height() - contentsRect().height();
    const int target = std::max(kBaseHeight, menuBar_->sizeHint().height() + border);
    if (target == height()) {
        return;
    }
    setFixedHeight(target);
    for (WindowGlyphButton* button : {minimizeButton_, maximizeButton_, closeButton_}) {
        button->setFixedSize(button->width(), target);
    }
}

bool TitleBar::event(QEvent* event) {
    // A font-scale change re-styles the menu bar, which invalidates this layout; that is the
    // moment the menu's size hint changes.
    if (event->type() == QEvent::LayoutRequest) {
        syncHeight();
    }
    return QFrame::event(event);
}

void TitleBar::showMaximizeGlyph(bool maximized) {
    maximizeButton_->setGlyph(maximized ? WindowGlyphButton::Glyph::Restore
                                        : WindowGlyphButton::Glyph::Maximize);
    maximizeButton_->setToolTip(maximized ? QStringLiteral("Restore") : QStringLiteral("Maximize"));
    maximizeButton_->setAccessibleName(maximized ? QStringLiteral("Restore window")
                                                 : QStringLiteral("Maximize window"));
}

void TitleBar::toggleMaximize() {
    if (window_ == nullptr) {
        return;
    }
    if (host_ != nullptr && host_->isCompactMode()) {
        return;
    }
    if (maximized_) {
        // Leave the maximized state itself: geometry set on a window the platform still holds
        // maximized does not stay put.
        window_->showNormal();
        if (normalGeometry_ && !normalGeometry_->isNull()) {
            window_->setGeometry(*normalGeometry_);
        }
        showMaximizeGlyph(false);
    } else {
        normalGeometry_ = window_->geometry();
        window_->showMaximized();
        showMaximizeGlyph(true);
    }
    maximized_ = !maximized_;
}

void TitleBar::syncMaximized(bool maximized) {
    if (maximized == maximized_) {
        return;
    }
    maximized_ = maximized;
    showMaximizeGlyph(maximized);
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window_ != nullptr) {
        const QPoint global = event->globalPosition().toPoint();
        if (host_ != nullptr) {
            const ResizeEdge edge = host_->resizeEdge(window_->mapFromGlobal(global));
            if (!edge.none()) {
                host_->beginResize(edge, global);
                event->accept();
                return;
            }
        }
        dragPosition_ = global - window_->frameGeometry().topLeft();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (host_ != nullptr && host_->isResizing()) {
        host_->applyResizeDelta(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    // A grab exactly at the window's top-left corner is a null QPoint, which Python's truth
    // test read as no drag; kept so the two behave alike.
    if (event->buttons() == Qt::LeftButton && dragPosition_ && !dragPosition_->isNull() &&
        window_ != nullptr) {
        if (host_ != nullptr) {
            host_->leaveMaximizedInPlace();
        }
        window_->move(event->globalPosition().toPoint() - *dragPosition_);
        event->accept();
        return;
    }
    QFrame::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window_ != nullptr) {
        dragPosition_.reset();
        if (host_ != nullptr && host_->isResizing()) {
            host_->finishResize();
            event->accept();
            return;
        }
    }
    QFrame::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        toggleMaximize();
    }
}

} // namespace fv
