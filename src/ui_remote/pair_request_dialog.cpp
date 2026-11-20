#include "ui_remote/pair_request_dialog.h"

#include <algorithm>
#include <cmath>

#include <QCloseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "remote/protocol.h"
#include "ui_remote/remote_text.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

QString q(const std::string& text) {
    return QString::fromStdString(text);
}

} // namespace

PairRequestDialog::PairRequestDialog(const remote::PairRequestInfo& request, QWidget* parent,
                                     const std::vector<std::string>& grants)
    : QDialog(parent), requestId_(request.id) {
    const QString name = request.name.empty() ? tr("A computer") : q(request.name);
    setObjectName(QStringLiteral("pairRequestDialog"));
    setWindowTitle(tr("Pair a computer"));
    setModal(false);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    setMinimumWidth(440);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(12);
    auto* title = new WrappedLabel(tr("%1 wants to use this computer's engine").arg(name));
    title->setObjectName(QStringLiteral("pairRequestTitle"));
    QFont font = title->font();
    font.setPointSizeF(font.pointSizeF() * 1.25);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    layout->addWidget(new WrappedLabel(
        tr("It's asking from %1. Allow it only if %2 shows this same number:")
            .arg(request.address.empty() ? tr("this network") : q(request.address), name)));
    code_ = new QLabel(q(remote::format_sas(request.sas)));
    code_->setObjectName(QStringLiteral("pairRequestCode"));
    code_->setAlignment(Qt::AlignCenter);
    code_->setAccessibleName(tr("Pairing number"));
    font = code_->font();
    font.setPointSizeF(font.pointSizeF() * 2.4);
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 4);
    code_->setFont(font);
    layout->addWidget(code_);
    expiry_ = new WrappedLabel;
    expiry_->setObjectName(QStringLiteral("pairRequestExpiry"));
    layout->addWidget(expiry_);
    grants_ = new WrappedLabel(q(remote_text::pairing_grants_phrase(grants)));
    grants_->setObjectName(QStringLiteral("pairRequestGrants"));
    layout->addWidget(grants_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    deny_ = neutralButton(new Button(tr("Deny")));
    deny_->setObjectName(QStringLiteral("pairRequestDenyButton"));
    connect(deny_, &QPushButton::clicked, this, [this] { answer(false); });
    // A plain Button: PrimaryButton's own large padding outranks the compact tone outside
    // Settings, and 34 px then cut its label off.
    allow_ = compactPrimaryButton(new Button(tr("Allow")));
    allow_->setObjectName(QStringLiteral("pairRequestAllowButton"));
    connect(allow_, &QPushButton::clicked, this, [this] { answer(true); });
    // Deny is the safe key to fall on.
    deny_->setDefault(true);
    buttons->addWidget(deny_);
    buttons->addWidget(allow_);
    layout->addLayout(buttons);

    const double left =
        std::isfinite(request.seconds_left) ? std::max(0.0, request.seconds_left) : 0.0;
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(left));
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &PairRequestDialog::tick);
    timer_->start();
    tick();
}

void PairRequestDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Opened a few pixels shorter than its own hint at larger font sizes, which squeezed the
    // buttons and cut their labels off.
    if (const int height = sizeHint().height(); height > this->height()) {
        resize(width(), height);
    }
}

void PairRequestDialog::tick() {
    const auto left = std::max<long long>(
        0,
        static_cast<long long>(
            std::chrono::duration<double>(deadline_ - std::chrono::steady_clock::now()).count()));
    expiry_->setText(tr("No answer by %1:%2 turns it down.")
                         .arg(left / 60)
                         .arg(left % 60, 2, 10, QLatin1Char('0')));
    if (left <= 0) {
        dismiss();
    }
}

void PairRequestDialog::answer(bool allow) {
    if (done_) {
        return;
    }
    done_ = true;
    timer_->stop();
    Q_EMIT answered(allow);
    close();
}

void PairRequestDialog::dismiss() {
    done_ = true;
    timer_->stop();
    close();
}

void PairRequestDialog::reject() {
    if (!done_) {
        answer(false);
        return;
    }
    QDialog::reject();
}

void PairRequestDialog::closeEvent(QCloseEvent* event) {
    if (!done_) {
        done_ = true;
        timer_->stop();
        Q_EMIT answered(false);
    }
    QDialog::closeEvent(event);
}

} // namespace fv
