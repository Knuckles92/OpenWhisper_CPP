#include "ui_personalize/snippets_panel.h"

#include <algorithm>
#include <exception>
#include <typeinfo>
#include <utility>

#include <QApplication>
#include <QBoxLayout>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "settings/store.h"
#include "text/personalize_copy.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.snippets_panel"};

constexpr int kIdRole = Qt::UserRole;
constexpr int kPreviewRole = Qt::UserRole + 1;
// QWIDGETSIZE_MAX: no maximum.
constexpr int kUnlimited = 16777215;

int scaledRound(double value) {
    return static_cast<int>(py::round_half_even(value));
}

// str.splitlines(): the line boundaries Python's str knows.
QStringList splitLines(const QString& text) {
    QStringList lines;
    QString line;
    for (qsizetype at = 0; at < text.size(); ++at) {
        const char16_t code = text.at(at).unicode();
        const bool boundary = code == u'\n' || code == u'\r' || code == u'\v' || code == u'\f' ||
                              code == 0x1c || code == 0x1d || code == 0x1e || code == 0x85 ||
                              code == 0x2028 || code == 0x2029;
        if (!boundary) {
            line += text.at(at);
            continue;
        }
        if (code == u'\r' && at + 1 < text.size() && text.at(at + 1) == u'\n') {
            ++at;
        }
        lines.append(line);
        line.clear();
    }
    if (!line.isEmpty()) {
        lines.append(line);
    }
    return lines;
}

// _first_line: the first line of what the snippet types that is not blank, stripped.
QString firstLine(const text::Snippet& snippet) {
    for (const QString& line : splitLines(qt::toQString(text::plain_text(snippet)))) {
        const std::string stripped = py::strip(qt::toUtf8(line));
        if (!stripped.empty()) {
            return qt::toQString(stripped);
        }
    }
    return {};
}

// _SnippetDelegate: paints a snippet as its trigger over a dimmer first line of its text. The
// row background (hover, selection) still comes from the stylesheet.
class SnippetDelegate : public QStyledItemDelegate {
public:
    static constexpr int kPadX = 10;
    static constexpr int kPadY = 8;
    static constexpr int kGap = 2;

    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        const auto [title, detail] = fonts(option);
        const int height = QFontMetrics(title).height() + kGap + QFontMetrics(detail).height();
        return {0, height + 2 * kPadY};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const QString trigger = item.text;
        item.text.clear();
        QStyle* style = item.widget != nullptr ? item.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);
        const bool selected = item.state.testFlag(QStyle::State_Selected);
        const auto [titleFont, detailFont] = fonts(item);
        const QRect area = item.rect.adjusted(kPadX, kPadY, -kPadX, -kPadY);
        painter->save();
        int top = area.top();
        const struct {
            QString text;
            QFont font;
            std::string_view color;
        } lines[] = {
            {trigger, titleFont, selected ? "text-heading" : "slate-text"},
            {index.data(kPreviewRole).toString(), detailFont,
             selected ? "accent-soft" : "slate-text-3"},
        };
        for (const auto& line : lines) {
            const QFontMetrics metrics(line.font);
            const QRect rect(area.left(), top, area.width(), metrics.height());
            painter->setFont(line.font);
            painter->setPen(ui::tokenColor(line.color));
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                              metrics.elidedText(line.text, Qt::ElideRight, area.width()));
            top += metrics.height() + kGap;
        }
        painter->restore();
    }

private:
    static std::pair<QFont, QFont> fonts(const QStyleOptionViewItem& option) {
        QFont title(option.font);
        title.setWeight(QFont::DemiBold);
        QFont detail(option.font);
        if (detail.pixelSize() > 0) {
            detail.setPixelSize(std::max(1, detail.pixelSize() - 1));
        } else {
            detail.setPointSizeF(std::max(1.0, detail.pointSizeF() - 0.75));
        }
        return {title, detail};
    }
};

// _field_label.
QLabel* fieldLabel(const QString& text, QWidget* buddy) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("textModelFieldLabel"));
    label->setBuddy(buddy);
    return label;
}

text::Snippet blankSnippet() {
    return {text::new_snippet_id(), "", "", false};
}

} // namespace

SnippetsPanel::SnippetsPanel(SettingsStore& store, QWidget* parent)
    : QWidget(parent), store_(store), saved_(blankSnippet()) {
    question = [](QWidget* parent, const QString& title, const QString& text,
                  QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
        return QMessageBox::question(parent, title, text, buttons, defaultButton);
    };
    setObjectName(QStringLiteral("snippetsPanel"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    snippetId_ = saved_.id;

    countLabel_ = new QLabel;
    countLabel_->setObjectName(QStringLiteral("snippetsCount"));
    countLabel_->setAlignment(Qt::AlignCenter);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    columns_ = new QBoxLayout(QBoxLayout::LeftToRight);
    columns_->setSpacing(18);
    root->addLayout(columns_);
    library_ = new QWidget;
    library_->setObjectName(QStringLiteral("snippetsLibrary"));
    columns_->addWidget(library_);
    editor_ = new QWidget;
    editor_->setObjectName(QStringLiteral("snippetsEditor"));
    columns_->addWidget(editor_, 1);
    buildLibrary();
    buildEditor();
    refresh();
}

SnippetsPanel::~SnippetsPanel() = default;

void SnippetsPanel::buildLibrary() {
    auto* column = new QVBoxLayout(library_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);
    snippetList_ = new QListWidget;
    snippetList_->setObjectName(QStringLiteral("snippetsList"));
    snippetList_->setAccessibleName(QStringLiteral("Snippets"));
    snippetList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    snippetList_->setUniformItemSizes(true);
    snippetList_->setItemDelegate(new SnippetDelegate(snippetList_));
    snippetList_->setMinimumHeight(120);
    connect(snippetList_, &QListWidget::currentItemChanged, this, &SnippetsPanel::selectionChanged);
    column->addWidget(snippetList_, 1);

    emptyState_ = new QFrame;
    emptyState_->setObjectName(QStringLiteral("snippetsEmpty"));
    auto* empty = new QVBoxLayout(emptyState_);
    empty->setContentsMargins(14, 14, 14, 14);
    empty->setSpacing(8);
    auto* title = new WrappedLabel(QStringLiteral("No snippets yet"));
    title->setObjectName(QStringLiteral("snippetsEmptyTitle"));
    empty->addWidget(title);
    auto* hint = new WrappedLabel(QStringLiteral("Start with one of these, or add your own."));
    hint->setObjectName(QStringLiteral("infoLabel"));
    empty->addWidget(hint);
    for (const std::string_view trigger : kSuggestedTriggers) {
        const QString label = qt::toQString(trigger);
        auto* button = neutralButton(new Button(label));
        // Full width in the column anyway; only a narrow column needs them to go down to their
        // label's own width.
        fitCompactButton(button, 0);
        button->setAccessibleName(QStringLiteral("Start a snippet for “%1”").arg(label));
        connect(button, &QPushButton::clicked, this, [this, label] { newSnippet(label); });
        empty->addWidget(button);
        suggestionButtons_.push_back(button);
    }
    empty->addStretch();
    column->addWidget(emptyState_, 1);

    newButton_ = compactPrimaryButton(new Button(QStringLiteral("New snippet")));
    connect(newButton_, &QPushButton::clicked, this, [this] { newSnippet(); });
    column->addWidget(newButton_);
    duplicateButton_ = neutralButton(new Button(QStringLiteral("Duplicate")));
    connect(duplicateButton_, &QPushButton::clicked, this, &SnippetsPanel::duplicateSnippet);
    deleteButton_ = neutralButton(new Button(QStringLiteral("Delete")));
    connect(deleteButton_, &QPushButton::clicked, this, &SnippetsPanel::deleteSnippet);
    // Wraps onto two rows rather than overlapping in a narrow column.
    column->addWidget(new ButtonRow({duplicateButton_, deleteButton_}));
}

void SnippetsPanel::buildEditor() {
    auto* column = new QVBoxLayout(editor_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);

    triggerEdit_ = new QLineEdit;
    triggerEdit_->setObjectName(QStringLiteral("snippetTriggerInput"));
    triggerEdit_->setMaxLength(static_cast<int>(text::MAX_TRIGGER_CHARS));
    triggerEdit_->setPlaceholderText(QStringLiteral("e.g. my calendar link"));
    triggerEdit_->setAccessibleName(QStringLiteral("Trigger phrase"));
    connect(triggerEdit_, &QLineEdit::textChanged, this, [this] { updateHints(); });
    column->addWidget(fieldLabel(QStringLiteral("Trigger phrase"), triggerEdit_));
    column->addWidget(triggerEdit_);
    triggerNote_ = new WrappedLabel;
    triggerNote_->setObjectName(QStringLiteral("snippetTriggerNote"));
    triggerNote_->setTextFormat(Qt::PlainText);
    triggerNote_->hide();
    column->addWidget(triggerNote_);
    column->addSpacing(4);

    textEdit_ = new QPlainTextEdit;
    textEdit_->setObjectName(QStringLiteral("snippetTextInput"));
    textEdit_->setPlaceholderText(
        QStringLiteral("The address, link, or sign-off to type for you…"));
    textEdit_->setAccessibleName(QStringLiteral("Text to insert"));
    textEdit_->setTabChangesFocus(true);
    textEdit_->setMinimumHeight(scaledRound(120 * ui::currentUiFontScale()));
    connect(textEdit_, &QPlainTextEdit::textChanged, this, [this] { updateHints(); });
    column->addWidget(fieldLabel(QStringLiteral("Text to insert"), textEdit_));
    column->addWidget(textEdit_, 1);
    column->addSpacing(4);

    auto* formatRow = new QWidget;
    formatRow->setObjectName(QStringLiteral("snippetsFormatRow"));
    auto* row = new QHBoxLayout(formatRow);
    row->setContentsMargins(0, 2, 0, 2);
    row->setSpacing(12);
    formatSwitch_ = new SettingsSwitch;
    formatSwitch_->setAccessibleName(QStringLiteral("Keep formatting"));
    connect(formatSwitch_, &QAbstractButton::toggled, this, [this] { updateHints(); });
    auto* copy = new QVBoxLayout;
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(2);
    auto* formatTitle = new WrappedLabel(QStringLiteral("Keep formatting"));
    formatTitle->setObjectName(QStringLiteral("snippetsFormatTitle"));
    copy->addWidget(formatTitle);
    auto* formatHint = new WrappedLabel(
        QStringLiteral("Write **bold**, *italic*, [links](https://…) and - lists. Apps that take "
                       "rich text paste them formatted; others get plain text."));
    formatHint->setObjectName(QStringLiteral("infoLabel"));
    formatHint->setTextFormat(Qt::PlainText);
    copy->addWidget(formatHint);
    row->addLayout(copy, 1);
    // At the right edge, beside the title, like every other Settings switch.
    row->addWidget(formatSwitch_, 0, Qt::AlignTop);
    column->addWidget(formatRow);
    column->addSpacing(4);

    footer_ = new QBoxLayout(QBoxLayout::LeftToRight);
    footer_->setSpacing(12);
    preview_ = new WrappedLabel;
    preview_->setObjectName(QStringLiteral("snippetPreview"));
    preview_->setTextFormat(Qt::PlainText);
    footer_->addWidget(preview_, 1);
    saveButton_ = compactPrimaryButton(new Button(QStringLiteral("Save snippet")));
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveSnippet(); });
    footer_->addWidget(saveButton_, 0, Qt::AlignRight);
    column->addLayout(footer_);
    message_ = new WrappedLabel;
    message_->setObjectName(QStringLiteral("snippetMessage"));
    message_->setTextFormat(Qt::PlainText);
    message_->hide();
    column->addWidget(message_);
}

void SnippetsPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

void SnippetsPanel::reflow() {
    const double scale = ui::currentUiFontScale();
    const int libraryWidth = scaledRound(kLibraryWidth * scale);
    const int spacing = columns_->spacing();
    const bool narrow = width() < libraryWidth + spacing + scaledRound(kEditorMinWidth * scale);
    columns_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    library_->setMinimumWidth(narrow ? 0 : libraryWidth);
    library_->setMaximumWidth(narrow ? kUnlimited : libraryWidth);
    const int editorWidth = narrow ? width() : width() - libraryWidth - spacing;
    footer_->setDirection(editorWidth < scaledRound(340 * scale) ? QBoxLayout::TopToBottom
                                                                 : QBoxLayout::LeftToRight);
    if (narrow && snippetList_->count() != 0) {
        // Stacked above the editor, the list shows about three rows and scrolls, instead of
        // pushing the editor off the page.
        const double rows = std::min(static_cast<double>(snippetList_->count()), 3.5);
        const int chrome = snippetList_->height() - snippetList_->viewport()->height();
        const int height =
            scaledRound(rows * snippetList_->sizeHintForRow(0) + std::max(chrome, 12));
        snippetList_->setMinimumHeight(height);
        snippetList_->setMaximumHeight(height);
    } else {
        snippetList_->setMinimumHeight(scaledRound(120 * scale));
        snippetList_->setMaximumHeight(kUnlimited);
    }
}

Json SnippetsPanel::settings() const {
    try {
        return *store_.snapshot();
    } catch (const std::exception& error) {
        kLog.warning("Could not load settings for the snippet library ({})", typeid(error).name());
        return Json::object();
    }
}

text::Snippet SnippetsPanel::draft() const {
    return {snippetId_, py::strip(qt::toUtf8(triggerEdit_->text())),
            py::strip(qt::toUtf8(textEdit_->toPlainText()), "\r\n"), formatSwitch_->isChecked()};
}

bool SnippetsPanel::hasUnsavedChanges() const {
    return draft() != saved_;
}

bool SnippetsPanel::saveBeforeSwitch() {
    if (!hasUnsavedChanges()) {
        return true;
    }
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Unsaved snippet"),
                 QStringLiteral("Save your changes to this snippet?"),
                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return saveSnippet();
    }
    return answer == QMessageBox::Discard;
}

bool SnippetsPanel::saveDraft(bool ask) {
    if (!hasUnsavedChanges() || saveSnippet() || !ask) {
        return true;
    }
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Unsaved snippet"),
                 QStringLiteral("%1 Discard your changes to this snippet?").arg(message_->text()),
                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return false;
    }
    refresh(saved_.id);
    return true;
}

void SnippetsPanel::refresh(std::optional<std::string> selectedId) {
    snippets_ = text::load_snippets(settings());
    countLabel_->setText(QStringLiteral("%1 / %2").arg(snippets_.size()).arg(text::MAX_SNIPPETS));
    emptyState_->setVisible(snippets_.empty());
    snippetList_->setVisible(!snippets_.empty());
    const bool keepDraft = !selectedId.has_value() && hasUnsavedChanges();
    const std::string wanted = selectedId.value_or(snippetId_);
    loading_ = true;
    struct Done {
        SnippetsPanel* panel;
        ~Done() {
            panel->loading_ = false;
            panel->reflow();
        }
    } done{this};
    snippetList_->clear();
    QListWidgetItem* selected = nullptr;
    for (const text::Snippet& snippet : snippets_) {
        const QString preview = firstLine(snippet);
        const QString trigger = qt::toQString(snippet.trigger);
        auto* item = new QListWidgetItem(trigger);
        item->setToolTip(trigger + QStringLiteral("\n") + preview);
        item->setData(kIdRole, qt::toQString(snippet.id));
        item->setData(kPreviewRole, preview);
        snippetList_->addItem(item);
        if (snippet.id == wanted) {
            selected = item;
        }
    }
    if (keepDraft) {
        if (selected != nullptr) {
            snippetList_->setCurrentItem(selected);
        }
        updateHints();
        return;
    }
    if (selected == nullptr && !snippets_.empty()) {
        selected = snippetList_->item(0);
    }
    if (selected != nullptr) {
        snippetList_->setCurrentItem(selected);
        showSnippet(snippet(qt::toUtf8(selected->data(kIdRole).toString())));
    } else {
        snippetList_->setCurrentRow(-1);
        showSnippet(blankSnippet());
    }
}

text::Snippet SnippetsPanel::snippet(const std::string& snippetId) const {
    const auto found = std::ranges::find_if(
        snippets_, [&snippetId](const text::Snippet& snippet) { return snippet.id == snippetId; });
    return found != snippets_.end() ? *found : blankSnippet();
}

void SnippetsPanel::showSnippet(const text::Snippet& snippet,
                                const std::optional<text::Snippet>& saved) {
    {
        const QSignalBlocker trigger(triggerEdit_);
        const QSignalBlocker body(textEdit_);
        const QSignalBlocker format(formatSwitch_);
        triggerEdit_->setText(qt::toQString(snippet.trigger));
        textEdit_->setPlainText(qt::toQString(snippet.text));
        formatSwitch_->setChecked(snippet.formatted);
    }
    snippetId_ = snippet.id;
    // Compared through the editor, so text it trims never reads as an edit.
    saved_ = saved.has_value() ? *saved : draft();
    const bool exists = std::ranges::any_of(
        snippets_, [&snippet](const text::Snippet& known) { return known.id == snippet.id; });
    deleteButton_->setEnabled(exists);
    duplicateButton_->setEnabled(exists);
    setMessage(QString());
    updateHints();
}

void SnippetsPanel::updateHints() {
    const text::Snippet current = draft();
    const std::string note = text::trigger_note(current.trigger, snippets_, current.id);
    const bool duplicate = text::find_duplicate(current.trigger, snippets_, current.id).has_value();
    triggerNote_->setText(qt::toQString(note));
    triggerNote_->setVisible(!note.empty());
    ui::setStyleProperty(triggerNote_, ui::prop::tone,
                         duplicate ? QStringLiteral("error") : QStringLiteral("hint"));
    preview_->setText(qt::toQString(text::snippets_page::preview(current)));
}

void SnippetsPanel::setMessage(const QString& text, bool error) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
    ui::setStyleProperty(message_, ui::prop::tone,
                         error ? QStringLiteral("error") : QStringLiteral("ok"));
}

void SnippetsPanel::selectionChanged(QListWidgetItem* current, QListWidgetItem* previous) {
    if (loading_ || current == nullptr) {
        return;
    }
    // Saving first rebuilds the list, which deletes `current`.
    const std::string snippetId = qt::toUtf8(current->data(kIdRole).toString());
    if (!saveBeforeSwitch()) {
        const QSignalBlocker blocker(snippetList_);
        snippetList_->setCurrentItem(previous);
        return;
    }
    refresh(snippetId);
}

bool SnippetsPanel::newSnippet(const QString& trigger) {
    if (!saveBeforeSwitch()) {
        return false;
    }
    {
        const QSignalBlocker blocker(snippetList_);
        snippetList_->setCurrentRow(-1);
    }
    const text::Snippet blank = blankSnippet();
    showSnippet({blank.id, qt::toUtf8(trigger), "", false}, blank);
    if (trigger.isEmpty()) {
        triggerEdit_->setFocus();
    } else {
        textEdit_->setFocus();
    }
    return true;
}

void SnippetsPanel::duplicateSnippet() {
    const text::Snippet source = draft();
    if (!saveBeforeSwitch()) {
        return;
    }
    snippets_ = text::load_snippets(settings());
    const std::string base = py::rstrip(
        py::slice(source.trigger, 0, static_cast<std::int64_t>(text::MAX_TRIGGER_CHARS) - 3));
    int number = 2;
    while (text::find_duplicate(base + " " + std::to_string(number), snippets_).has_value()) {
        ++number;
    }
    {
        const QSignalBlocker blocker(snippetList_);
        snippetList_->setCurrentRow(-1);
    }
    const text::Snippet blank = blankSnippet();
    showSnippet({blank.id, base + " " + std::to_string(number), source.text, source.formatted},
                blank);
    triggerEdit_->setFocus();
    triggerEdit_->selectAll();
}

bool SnippetsPanel::saveSnippet() {
    const text::Snippet current = draft();
    try {
        store_.mutate([&current](Json& settings) { text::save_snippet(settings, current); });
    } catch (const py::ValueError& refused) {
        setMessage(qt::toQString(refused.what()), true);
        return false;
    } catch (const std::exception& error) {
        kLog.warning("Could not save the snippet ({})", typeid(error).name());
        setMessage(QStringLiteral("Couldn't save the snippet: %1").arg(qt::toQString(error.what())),
                   true);
        return false;
    }
    saved_ = current;
    refresh(current.id);
    setMessage(QStringLiteral("Saved. Say “%1” to insert it.").arg(qt::toQString(current.trigger)));
    Q_EMIT snippetsChanged();
    return true;
}

void SnippetsPanel::deleteSnippet() {
    const auto found = std::ranges::find_if(
        snippets_, [this](const text::Snippet& known) { return known.id == snippetId_; });
    if (found == snippets_.end()) {
        return;
    }
    const text::Snippet doomed = *found;
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Delete snippet?"),
                 QStringLiteral("Delete “%1”? Saying it will no longer insert its text.")
                     .arg(qt::toQString(doomed.trigger)),
                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    try {
        store_.mutate([&doomed](Json& settings) { text::delete_snippet(settings, doomed.id); });
    } catch (const std::exception& error) {
        kLog.warning("Could not delete the snippet ({})", typeid(error).name());
        setMessage(
            QStringLiteral("Couldn't delete the snippet: %1").arg(qt::toQString(error.what())),
            true);
        return;
    }
    refresh(std::string());
    Q_EMIT snippetsChanged();
}

} // namespace fv
