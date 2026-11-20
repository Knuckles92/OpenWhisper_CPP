#include "ui_widgets/dictation_languages_field.h"

#include <algorithm>
#include <exception>

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/flow_layout.h"
#include "ui_widgets/speech_backend_picker.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.local_engine_controls"};

constexpr std::string_view kAuto = "auto";

// Holds the flow of chips and reports the height they need at its width. Like WrappedLabel, its
// size hints carry the wrapped height: Qt's height-for-width path estimates nested cards at the
// wrong width.
class ChipArea : public QWidget {
public:
    using QWidget::QWidget;

    QSize sizeHint() const override {
        const QSize widest = layout()->minimumSize();
        const int available = width();
        if (available <= 0) {
            return widest;
        }
        return QSize(widest.width(), layout()->heightForWidth(available));
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (event->oldSize().width() != event->size().width()) {
            updateGeometry();
        }
    }
};

bool contains(const std::vector<std::string>& codes, const std::string& code) {
    return std::ranges::find(codes, code) != codes.end();
}

} // namespace

LanguageChip::LanguageChip(const QString& code, bool available, bool active,
                           const QString& unavailableReason, QWidget* parent)
    : QFrame(parent), code_(code) {
    const QString name = qt::toQString(speech::label(code.toStdString()));
    setObjectName(QStringLiteral("dictationLanguageChip"));
    ui::setStyleProperty(this, ui::prop::available, available);
    ui::setStyleProperty(this, ui::prop::active, active);
    setFixedHeight(30);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (!available) {
        setToolTip(unavailableReason);
    }
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 5, 0);
    layout->setSpacing(4);
    label_ = new QLabel(name);
    label_->setObjectName(QStringLiteral("dictationLanguageChipLabel"));
    layout->addWidget(label_);
    removeButton_ = new QToolButton;
    removeButton_->setObjectName(QStringLiteral("dictationLanguageChipRemove"));
    removeButton_->setIcon(ui::designIcon(QStringLiteral("x-gray.svg")));
    removeButton_->setIconSize(QSize(13, 13));
    removeButton_->setFixedSize(22, 22);
    removeButton_->setCursor(Qt::PointingHandCursor);
    removeButton_->setToolTip(QStringLiteral("Remove %1").arg(name));
    removeButton_->setAccessibleName(QStringLiteral("Remove %1").arg(name));
    connect(removeButton_, &QToolButton::clicked, this, [this] { Q_EMIT removed(code_); });
    layout->addWidget(removeButton_);
}

DictationLanguagesField::DictationLanguagesField(SettingsStore& settings, QWidget* parent)
    : QWidget(parent), settings_(settings), addMenu_(new QMenu(this)) {
    setObjectName(QStringLiteral("dictationLanguages"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    chipArea_ = new ChipArea;
    chipArea_->setObjectName(QStringLiteral("dictationLanguageChips"));
    flow_ = new FlowLayout(chipArea_);
    // "Add" beside the plus fits a 460 px tiled window at large fonts. The button belongs to the
    // chip area from the start: it leaves the flow, but not the widget tree, while the engine
    // takes one language.
    addButton_ = neutralButton(new Button(QStringLiteral("Add"), chipArea_));
    addButton_->setIcon(ui::designIcon(QStringLiteral("plus-blue.svg")));
    addButton_->setToolTip(QStringLiteral("Add a language"));
    addButton_->setAccessibleName(QStringLiteral("Add a language"));
    connect(addButton_, &QPushButton::clicked, this, [this] { openAddMenu(); });
    layout->addWidget(chipArea_);
    caption_ = new WrappedLabel(QString());
    caption_->setObjectName(QStringLiteral("infoLabel"));
    layout->addWidget(caption_);
    refresh();
}

void DictationLanguagesField::setBackend(const QString& backend) {
    backend_ = backend;
    refresh();
}

Json DictationLanguagesField::savedSettings() const {
    Json settings = *settings_.snapshot();
    if (!settings.is_object()) {
        settings = Json::object();
    }
    if (!backend_.isEmpty()) {
        settings[std::string(keys::selected_model.key())] = backend_.toStdString();
    }
    return settings;
}

void DictationLanguagesField::refresh() {
    const Json settings = savedSettings();
    const std::vector<std::string> chosen = speech::chosen_languages(settings);
    const std::vector<std::string> accepted = speech::accepted_languages(settings);
    const std::vector<std::string> choices = speech::language_choices(settings);
    const std::string reason = speech::single_language_reason(settings);
    const std::string active = speech::job_language(settings);
    const QString engineName = backendDisplayName(speech::engine(settings));
    const QString unavailable = QStringLiteral("%1 can't use this language.").arg(engineName);

    flow_->removeWidget(addButton_);
    for (LanguageChip* chip : chips_) {
        flow_->removeWidget(chip);
        chip->hide();
        chip->deleteLater();
    }
    chips_.clear();
    if (reason.empty()) {
        for (const std::string& code : chosen) {
            auto* chip =
                new LanguageChip(qt::toQString(code), contains(accepted, code),
                                 code == active && choices.size() > 1, unavailable, chipArea_);
            connect(chip, &LanguageChip::removed, this, &DictationLanguagesField::remove);
            flow_->addWidget(chip);
            chips_.push_back(chip);
        }
        flow_->addWidget(addButton_);
    }
    chipArea_->setVisible(reason.empty());
    remaining_.clear();
    for (const std::string& code : accepted) {
        if (!contains(chosen, code)) {
            remaining_.push_back(code);
        }
    }
    addButton_->setEnabled(!remaining_.empty());
    const std::string current = speech::current_language(settings);
    caption_->setText(reason.empty() ? captionText(chosen, choices, current, engineName)
                                     : qt::toQString(reason));
    chipArea_->updateGeometry();
}

QString DictationLanguagesField::captionText(const std::vector<std::string>& chosen,
                                             const std::vector<std::string>& choices,
                                             const std::string& current,
                                             const QString& engineName) {
    const bool detects = current == kAuto;
    const QString uses = detects
                             ? QStringLiteral("detects the language")
                             : QStringLiteral("uses %1").arg(qt::toQString(speech::label(current)));
    if (chosen.empty()) {
        return QStringLiteral("Dictation %1. Add languages to switch between them "
                              "from the overlay, the tray or a shortcut.")
            .arg(uses);
    }
    if (choices.empty()) {
        return QStringLiteral("%1 can't use these, so dictation %2.").arg(engineName, uses);
    }
    if (choices.size() == 1) {
        return QStringLiteral("Dictation %1. Add another to switch between them.").arg(uses);
    }
    if (detects) {
        return QStringLiteral("Dictation %1. Switch from the overlay, the tray or a shortcut.")
            .arg(uses);
    }
    return QStringLiteral("Now dictating in %1. Switch from the overlay, the tray or a shortcut.")
        .arg(qt::toQString(speech::label(current)));
}

void DictationLanguagesField::openAddMenu() {
    addMenu_->clear();
    for (const std::string& code : remaining_) {
        QAction* action = addMenu_->addAction(qt::toQString(speech::label(code)));
        const QString chosen = qt::toQString(code);
        connect(action, &QAction::triggered, this, [this, chosen] { add(chosen); });
    }
    addMenu_->popup(addButton_->mapToGlobal(QPoint(0, addButton_->height())));
}

void DictationLanguagesField::add(const QString& code) {
    const std::string chosen = code.toStdString();
    save([&chosen](Json& settings) { speech::add_chosen(settings, chosen); });
}

void DictationLanguagesField::remove(const QString& code) {
    std::vector<std::string> codes;
    for (std::string& other : speech::chosen_languages(savedSettings())) {
        if (other != code.toStdString()) {
            codes.push_back(std::move(other));
        }
    }
    save([&codes](Json& settings) { speech::set_chosen(settings, codes); });
}

void DictationLanguagesField::save(const std::function<void(Json&)>& change) {
    try {
        settings_.mutate(change);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't save the dictation languages");
        caption_->setText(QStringLiteral("Couldn't save your languages. Try again."));
        return;
    }
    refresh();
    Q_EMIT changed();
}

} // namespace fv
