#include "ui_widgets/searchable_combo.h"

#include <algorithm>
#include <optional>
#include <string>

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTimer>

#include "pyport/text.h"
#include "ui_theme/local_style.h"
#include "uimath/fuzzy_match.h"

namespace fv {
namespace {

// Spaced as in OpenWhisper: the widget's tree carries the sheet verbatim.
const char* const kBadgeSheet = R"(
            QLabel#searchableComboBadge {
                color: @success-text-strong;
                background-color: rgba(@success-rgb, 0.16);
                border: 1px solid rgba(@success-rgb, 0.45);
                border-radius: 8px;
                padding: 0px 7px;
                font-size: 10px;
                font-weight: 700;
            }
        )";

QString pyStrip(const QString& text) {
    return QString::fromStdString(py::strip(text.toStdString()));
}

// Plain text-producing keys (no Ctrl or Alt), as str.isprintable judges the text.
bool isPrintableKey(const QKeyEvent* event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        return false;
    }
    const QString text = event->text();
    return !text.isEmpty() && py::isprintable(text.toStdString());
}

// Text input, editing keys and the standard edit shortcuts.
bool isTextEditingKey(const QKeyEvent* event) {
    if (isPrintableKey(event)) {
        return true;
    }
    switch (event->key()) {
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
        return true;
    default:
        break;
    }
    if (!(event->modifiers() & Qt::ControlModifier)) {
        return false;
    }
    switch (event->key()) {
    case Qt::Key_A:
    case Qt::Key_C:
    case Qt::Key_V:
    case Qt::Key_X:
    case Qt::Key_Y:
    case Qt::Key_Z:
        return true;
    default:
        return false;
    }
}

} // namespace

SearchableComboBox::SearchableComboBox(QWidget* parent) : NoWheelComboBox(parent) {
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    QLineEdit* editor = lineEdit();
    connect(editor, &QLineEdit::textEdited, this, &SearchableComboBox::onTextEdited);
    // Qt keeps keyboard focus on the combo frame, even for an editable combo. Those keys are
    // routed to the line edit here, so the combo's built-in type-selection never commits a
    // match mid-search.
    editor->installEventFilter(this);
    connect(this, &QComboBox::activated, this, [this](int) { ++activationCount_; });
    connect(this, &QComboBox::textActivated, this,
            [this](const QString&) { ++textActivationCount_; });

    // Created after the line edit so it stacks above the editor. The application stylesheet
    // never reaches a QComboBox's own children, so the badge carries its appearance itself.
    badge_ = new QLabel(QString(), this);
    badge_->setObjectName(QStringLiteral("searchableComboBadge"));
    badge_->setAlignment(Qt::AlignCenter);
    badge_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    ui::setLocalStyleSheet(badge_, QString::fromUtf8(kBadgeSheet));
    badge_->hide();
}

void SearchableComboBox::setBadge(const QString& text) {
    const QString stripped = pyStrip(text);
    if (stripped == badgeText()) {
        return;
    }
    badge_->setText(stripped);
    badge_->setVisible(!stripped.isEmpty());
    layoutBadge();
}

QString SearchableComboBox::badgeText() const {
    return badge_->text();
}

void SearchableComboBox::resizeEvent(QResizeEvent* event) {
    NoWheelComboBox::resizeEvent(event);
    layoutBadge();
}

void SearchableComboBox::layoutBadge() {
    QLineEdit* editor = lineEdit();
    if (badge_->text().isEmpty()) {
        if (editor != nullptr) {
            editor->setTextMargins(0, 0, 0, 0);
        }
        return;
    }
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QRect field =
        style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
    const int width = badge_->sizeHint().width();
    const int height = std::min(kBadgeHeight, field.height());
    badge_->setGeometry(field.right() - width, field.center().y() - height / 2 + 1, width, height);
    if (editor != nullptr) {
        editor->setTextMargins(0, 0, width + kBadgeGap, 0);
    }
}

void SearchableComboBox::showPopup() {
    if (!filtering_) {
        applyFilter(QString());
        searchStarted_ = false;
    }
    NoWheelComboBox::showPopup();
    installPopupKeyFilters();
}

bool SearchableComboBox::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress) {
        return NoWheelComboBox::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    const bool popupVisible = view()->window()->isVisible();
    if (watched == lineEdit()) {
        if (popupVisible && handlePopupNavigation(key)) {
            return true;
        }
        if (!searchStarted_ && isPrintableKey(key)) {
            lineEdit()->selectAll();
            searchStarted_ = true;
        }
    } else if (isPopupWidget(watched)) {
        if (handlePopupNavigation(key)) {
            return true;
        }
        if (isTextEditingKey(key)) {
            forwardToLineEdit(key);
            return true;
        }
    }
    return NoWheelComboBox::eventFilter(watched, event);
}

void SearchableComboBox::keyPressEvent(QKeyEvent* event) {
    if (view()->window()->isVisible() && handlePopupNavigation(event)) {
        return;
    }
    if (isTextEditingKey(event)) {
        forwardToLineEdit(event);
        event->accept();
        return;
    }
    NoWheelComboBox::keyPressEvent(event);
}

bool SearchableComboBox::handlePopupNavigation(QKeyEvent* event) {
    const int key = event->key();
    QAbstractItemView* list = view();
    QAbstractItemModel* items = model();
    if (key == Qt::Key_Escape) {
        hidePopup();
        event->accept();
        return true;
    }
    if (key == Qt::Key_Down || key == Qt::Key_Up) {
        const int current = list->currentIndex().row();
        const int step = key == Qt::Key_Down ? 1 : -1;
        const int rows = items->rowCount();
        int row = current >= 0 ? current : -1;
        auto* rowsView = qobject_cast<QListView*>(list);
        for (int i = 0; i < rows; ++i) {
            row = ((row + step) % rows + rows) % rows;
            if (rowsView == nullptr || !rowsView->isRowHidden(row)) {
                setHighlightedRow(row);
                break;
            }
        }
        event->accept();
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        const QModelIndex index = list->currentIndex();
        auto* rowsView = qobject_cast<QListView*>(list);
        if (index.isValid() && (rowsView == nullptr || !rowsView->isRowHidden(index.row()))) {
            const QString text = items->data(index).toString();
            const int activationCount = activationCount_;
            const int textActivationCount = textActivationCount_;
            setCurrentText(text);
            hidePopup();
            // Cocoa emits the native activation even though this filter accepts Return; Windows
            // and Linux do not. Once Qt has finished dispatching the key, only the signals the
            // platform did not emit are filled in.
            const int row = index.row();
            QTimer::singleShot(0, this, [this, row, text, activationCount, textActivationCount] {
                finishKeyActivation(row, text, activationCount, textActivationCount);
            });
            event->accept();
            return true;
        }
    }
    return false;
}

void SearchableComboBox::finishKeyActivation(int row, const QString& text, int activationCount,
                                             int textActivationCount) {
    if (activationCount_ == activationCount) {
        Q_EMIT activated(row);
    }
    if (textActivationCount_ == textActivationCount) {
        Q_EMIT textActivated(text);
    }
}

void SearchableComboBox::onTextEdited(const QString& text) {
    searchStarted_ = true;
    applyFilter(text);
    if (text.isEmpty() || view()->window()->isVisible()) {
        return;
    }
    filtering_ = true;
    showPopup();
    filtering_ = false;
}

void SearchableComboBox::applyFilter(const QString& text) {
    auto* rowsView = qobject_cast<QListView*>(view());
    QAbstractItemModel* items = model();
    const std::string needle = py::strip(text.toStdString());
    int bestRow = -1;
    int bestScore = -1;
    for (int row = 0; row < items->rowCount(); ++row) {
        if (needle.empty()) {
            if (rowsView != nullptr) {
                rowsView->setRowHidden(row, false);
            }
            if (bestRow < 0) {
                bestRow = row;
            }
            continue;
        }
        const std::string itemText = items->data(items->index(row, 0)).toString().toStdString();
        const std::optional<int> score = fuzzy_score(needle, itemText);
        if (rowsView != nullptr) {
            rowsView->setRowHidden(row, !score.has_value());
        }
        if (score && *score > bestScore) {
            bestScore = *score;
            bestRow = row;
        }
    }
    if (bestRow >= 0) {
        setHighlightedRow(bestRow);
    }
}

void SearchableComboBox::setHighlightedRow(int row) {
    QAbstractItemView* list = view();
    QItemSelectionModel* selection = list->selectionModel();
    const bool viewWasBlocked = list->blockSignals(true);
    const bool selectionWasBlocked = selection->blockSignals(true);
    list->setCurrentIndex(model()->index(row, 0));
    selection->blockSignals(selectionWasBlocked);
    list->blockSignals(viewWasBlocked);
}

void SearchableComboBox::installPopupKeyFilters() {
    QAbstractItemView* list = view();
    QWidget* popup = list->window();
    if (popup != nullptr && popup != this && !popupFilterInstalled_) {
        popup->installEventFilter(this);
        list->installEventFilter(this);
        popupFilterInstalled_ = true;
    }
}

bool SearchableComboBox::isPopupWidget(const QObject* object) const {
    const QAbstractItemView* list = view();
    return object == list || object == list->window();
}

void SearchableComboBox::forwardToLineEdit(const QKeyEvent* event) {
    QLineEdit* editor = lineEdit();
    if (editor == nullptr) {
        return;
    }
    QKeyEvent forwarded(event->type(), event->key(), event->modifiers(), event->text(),
                        event->isAutoRepeat(), static_cast<quint16>(event->count()));
    QCoreApplication::sendEvent(editor, &forwarded);
}

} // namespace fv
