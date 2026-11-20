#include "ui_widgets/engine_field.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QConicalGradient>
#include <QPainter>
#include <QPen>
#include <QVBoxLayout>

#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/field_help.h"

namespace fv {
namespace {

const QString kDotStyle = QStringLiteral("background: transparent; border: none;");

const char* dotToken(EngineStatus status) {
    switch (status) {
    case EngineStatus::Ready:
        return "success";
    case EngineStatus::Attention:
        return "warning";
    case EngineStatus::Unknown:
        break;
    }
    return "text-muted";
}

// An engine field's combo: ElidingComboBox with the engine card's narrower floor. It adds no
// signal or property, so it stays an ElidingComboBox to the theme and to widget dumps.
class FieldCombo : public ElidingComboBox {
public:
    using ElidingComboBox::ElidingComboBox;

    QSize minimumSizeHint() const override {
        const QSize hint = QComboBox::minimumSizeHint();
        return QSize(std::min(hint.width(), kFieldMinimumWidth), hint.height());
    }
};

} // namespace

StatusDot::StatusDot(QWidget* parent, int diameter) : QLabel(parent), timer_(this) {
    setObjectName(QStringLiteral("engineStatusDot"));
    ui::setLocalStyleSheet(this, kDotStyle);
    setFixedSize(diameter, diameter);
    timer_.setInterval(33);
    connect(&timer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void StatusDot::setStatus(EngineStatus status) {
    status_ = status;
    update();
}

void StatusDot::setBusy(bool busy) {
    busy_ = busy;
    if (busy && !clock_.isValid()) {
        clock_.start();
    }
    syncAnimation();
    update();
}

void StatusDot::syncAnimation() {
    if (busy_ && isVisible()) {
        if (!timer_.isActive()) {
            timer_.start();
        }
    } else {
        timer_.stop();
    }
}

void StatusDot::showEvent(QShowEvent* event) {
    QLabel::showEvent(event);
    syncAnimation();
}

void StatusDot::hideEvent(QHideEvent* event) {
    QLabel::hideEvent(event);
    timer_.stop();
}

void StatusDot::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QPointF center = QRectF(rect()).center();
    if (!busy_) {
        painter.setBrush(ui::tokenColor(dotToken(status_)));
        painter.drawEllipse(center, kDotDiameter / 2.0, kDotDiameter / 2.0);
        return;
    }

    const double phase = static_cast<double>(clock_.elapsed()) / 1200.0;
    const QRectF ring = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ui::tokenColor("accent", 40), 2));
    painter.drawEllipse(ring);
    QConicalGradient tail(center, -phase * 360);
    tail.setColorAt(0, ui::tokenColor("accent-cyan"));
    tail.setColorAt(0.18, ui::tokenColor("accent"));
    tail.setColorAt(0.75, ui::tokenColor("accent", 0));
    tail.setColorAt(1, ui::tokenColor("accent", 0));
    painter.setPen(QPen(QBrush(tail), 2));
    painter.drawEllipse(ring);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ui::tokenColor(
        "accent-cyan", static_cast<int>(130 + 70 * std::sin(phase * 2 * std::numbers::pi))));
    painter.drawEllipse(center, 1.6, 1.6);
}

StatusFieldCombo::StatusFieldCombo(QWidget* parent)
    : ElidingComboBox(parent), dot_(new StatusDot(this)) {
    positionDot();
}

void StatusFieldCombo::setStatus(EngineStatus status) {
    dot_->setStatus(status);
}

void StatusFieldCombo::setStatusVisible(bool visible) {
    dot_->setVisible(visible);
    ui::setStyleProperty(this, ui::prop::dot,
                         visible ? QStringLiteral("true") : QStringLiteral("false"));
}

QSize StatusFieldCombo::minimumSizeHint() const {
    const QSize hint = QComboBox::minimumSizeHint();
    return QSize(std::min(hint.width(), kFieldMinimumWidth), hint.height());
}

void StatusFieldCombo::resizeEvent(QResizeEvent* event) {
    ElidingComboBox::resizeEvent(event);
    positionDot();
}

void StatusFieldCombo::positionDot() {
    dot_->move(kDotLeft, (height() - dot_->height()) / 2);
}

QComboBox* engineCombo(const QStringList& items, bool primary) {
    QComboBox* combo = primary ? static_cast<QComboBox*>(new StatusFieldCombo)
                               : static_cast<QComboBox*>(new FieldCombo);
    combo->addItems(items);
    combo->setObjectName(primary ? QStringLiteral("engineFieldPrimary")
                                 : QStringLiteral("engineField"));
    return combo;
}

QString deviceLabel(const QString& device) {
    if (device == QLatin1String("auto")) {
        return QStringLiteral("Auto");
    }
    if (device == QLatin1String("cuda")) {
        return QStringLiteral("GPU");
    }
    if (device == QLatin1String("cpu")) {
        return QStringLiteral("CPU");
    }
    return device;
}

void setDeviceChoices(QComboBox* combo, const QStringList& devices) {
    combo->clear();
    for (const QString& device : devices) {
        combo->addItem(deviceLabel(device), device);
    }
}

QWidget* engineField(const QString& caption, QWidget* field, const QString& helpText,
                     const std::vector<FieldHelpLink>& links,
                     std::function<void(const QString&)> onHelp) {
    auto* wrapper = new QWidget;
    wrapper->setObjectName(QStringLiteral("engineFieldGroup"));
    auto* column = new QVBoxLayout(wrapper);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);

    QWidget* label = nullptr;
    if (!helpText.isEmpty()) {
        auto* help = new FieldHelp(caption, caption, helpText, links, wrapper);
        if (onHelp) {
            QObject::connect(help, &FieldHelp::destinationRequested, help, std::move(onHelp));
        }
        label = help;
    } else {
        auto* plain = new QLabel(caption);
        plain->setObjectName(QStringLiteral("engineFieldLabel"));
        label = plain;
    }
    column->addWidget(label);
    column->addWidget(field);
    return wrapper;
}

} // namespace fv
