#include "ui_personalize/dictionary_page.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <typeinfo>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "settings/keys.h"
#include "speech/endpoints.h"
#include "text/cleanup_prompts.h"
#include "text/recognition.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/dictionary_library.h"
#include "ui_personalize/personalize_host.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dictionary"};

constexpr const char* kGenericEngineLine =
    "Engines that take hints also listen for your words. Every "
    "engine fixes spellings after recognition.";
constexpr const char* kSteerOffLine =
    "Off. Your dictionary still fixes spellings after recognition and in AI cleanup.";
constexpr const char* kSteerOffNoCleanupLine =
    "Off. Only Sounds like spellings are fixed while AI cleanup is off.";
constexpr const char* kNotFixedHint =
    "Without a Sounds like spelling it won't be fixed while AI cleanup is off.";
constexpr const char* kLearnLine =
    "When you fix a dictated word the same way twice, it's added here. Put back a word AI "
    "cleanup changed and it's added at once, and cleanup leaves it alone. Only the words you "
    "just dictated are compared.";
constexpr const char* kLearnGateLine = "Off · needs Read text near the cursor";

// SEARCH_FIELDS: (control, title, description).
struct Field {
    const char* target;
    const char* title;
    const char* description;
};
constexpr Field kComposerField{
    "dictionary_composer_tile", "Add a word",
    "A name or term, and what it sounds like when dictation gets it wrong."};
constexpr Field kLibraryField{"dictionary_library_tile", "Your words",
                              "Starred words come first when an engine takes only a few hints."};
constexpr Field kSteerField{"dictionary_steer_tile", "Steer the speech model", kGenericEngineLine};
constexpr Field kLearnField{"dictionary_learn_tile", "Learn from my corrections", kLearnLine};

QString q(const char* text) {
    return QString::fromUtf8(text);
}

// CONTROL_ATTRS.
const std::vector<std::string>& controlNames() {
    static const std::vector<std::string> names{
        "dictionary_composer_tile",     "dictionary_term_edit",     "dictionary_heard_edit",
        "dictionary_add_button",        "dictionary_cancel_button", "dictionary_message",
        "dictionary_new_tile",          "dictionary_rules_tile",    "dictionary_library_tile",
        "dictionary_library",           "dictionary_steer_tile",    "dictionary_steer_switch",
        "dictionary_learn_tile",        "dictionary_learn_switch",  "dictionary_learn_gate_tile",
        "dictionary_cleanup_gate_tile",
    };
    return names;
}

std::function<BackendInfo()>& backendProvider() {
    static std::function<BackendInfo()> provider;
    return provider;
}

// _current_backend.
std::optional<BackendInfo> currentBackend() {
    const std::function<BackendInfo()>& provider = backendProvider();
    if (!provider) {
        return std::nullopt;
    }
    try {
        return provider();
    } catch (const std::exception& error) {
        kLog.debug("The current backend is unavailable ({})", typeid(error).name());
        return std::nullopt;
    }
}

bool cleanupOn(const Json& settings) {
    return text::resolve_level(settings) != text::CleanupLevel::None;
}

const Json& orEmpty(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
}

// _add_action: under the description rather than beside it, where a narrow window or a large
// font would push it past the tile's edge.
void addTileAction(InfoTile* tile, QWidget* button) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(button);
    row->addStretch();
    tile->addBodyLayout(row);
}

QString curlyQuoted(const std::string& term) {
    return QStringLiteral("“%1”").arg(qt::toQString(term));
}

} // namespace

DictionaryPage::DictionaryPage(PersonalizeHost& host, SettingsPage& container, CleanupGates& gates)
    : PersonalizePage(host, container), gates_(gates) {
    QVBoxLayout* layout = pageLayout();

    // Shown only while AI cleanup is off on an engine that can't take your words, when Sounds
    // like spellings are all that's fixed.
    cleanupGate_ = gates_.gateTile(
        QStringLiteral("Only Sounds like spellings get fixed. AI cleanup uses your words too."),
        [this] {
            return text::dictionary_page::cannot_steer(engineInfo(this->host().settings()));
        });
    ui::setStyleProperty(cleanupGate_, ui::prop::tileId, QStringLiteral("dictionaryCleanupGate"));
    // After the gates' own handler has turned cleanup on.
    connect(gates_.turnOnButton(cleanupGate_), &QPushButton::clicked, this,
            [this] { showSettings(this->host().settings()); });
    layout->addWidget(cleanupGate_);

    composer_ = new InfoTile(q(kComposerField.title), q(kComposerField.description),
                             ui::designIcon(QStringLiteral("plus-blue.svg")));
    ui::setStyleProperty(composer_, ui::prop::tileId, QStringLiteral("dictionaryComposer"));
    termEdit_ = new QLineEdit;
    termEdit_->setObjectName(QStringLiteral("dictionaryTermInput"));
    termEdit_->setPlaceholderText(QStringLiteral("Word or name"));
    termEdit_->setMaxLength(static_cast<int>(text::MAX_TERM_CHARS));
    termEdit_->setAccessibleName(QStringLiteral("Word or name"));
    connect(termEdit_, &QLineEdit::returnPressed, this, &DictionaryPage::submit);
    connect(termEdit_, &QLineEdit::textEdited, this, [this] { say(QString()); });
    composer_->addBody(termEdit_);
    heardEdit_ = new QLineEdit;
    heardEdit_->setObjectName(QStringLiteral("dictionaryHeardInput"));
    heardEdit_->setPlaceholderText(QStringLiteral("Sounds like (optional)"));
    heardEdit_->setToolTip(QStringLiteral("How dictation sometimes writes it. Separate several "
                                          "with commas; each is replaced with your word."));
    heardEdit_->setAccessibleName(QStringLiteral("Sounds like"));
    connect(heardEdit_, &QLineEdit::returnPressed, this, &DictionaryPage::submit);
    connect(heardEdit_, &QLineEdit::textEdited, this, [this] { say(QString()); });
    cancelButton_ = neutralButton(new Button(QStringLiteral("Cancel")));
    cancelButton_->setObjectName(QStringLiteral("dictionaryCancelButton"));
    fitCompactButton(cancelButton_);
    connect(cancelButton_, &QPushButton::clicked, this, &DictionaryPage::cancelEdit);
    cancelButton_->hide();
    addButton_ = compactPrimaryButton(new Button(QStringLiteral("Add")));
    addButton_->setObjectName(QStringLiteral("dictionaryAddButton"));
    fitCompactButton(addButton_);
    connect(addButton_, &QPushButton::clicked, this, &DictionaryPage::submit);
    composer_->addBody(new Reflow(heardEdit_, actionGroup({cancelButton_, addButton_}), 220));
    message_ = new WrappedLabel;
    message_->setObjectName(QStringLiteral("dictionaryMessage"));
    message_->hide();
    composer_->addBody(message_);
    addTileGroup(QString(), {composer_}, 1);

    newTile_ = new InfoTile(QStringLiteral("New from your corrections"), QString(),
                            ui::designIcon(QStringLiteral("wand-purple.svg")));
    ui::setStyleProperty(newTile_, ui::prop::tileId, QStringLiteral("dictionaryNew"));
    keepButton_ = neutralButton(new Button(QStringLiteral("Keep all")));
    keepButton_->setObjectName(QStringLiteral("dictionaryKeepNewButton"));
    keepButton_->setToolTip(QStringLiteral("Keep every new word and clear the New badges"));
    fitCompactButton(keepButton_);
    connect(keepButton_, &QPushButton::clicked, this, &DictionaryPage::keepNew);
    addTileAction(newTile_, keepButton_);
    newTile_->hide();
    layout->addWidget(newTile_);

    rulesTile_ = new InfoTile(QStringLiteral("Spellings in Learned rules"), QString(),
                              ui::designIcon(QStringLiteral("stack-purple.svg")));
    ui::setStyleProperty(rulesTile_, ui::prop::tileId, QStringLiteral("dictionaryRules"));
    moveButton_ = neutralButton(new Button(QStringLiteral("Move here")));
    moveButton_->setObjectName(QStringLiteral("dictionaryMoveRulesButton"));
    fitCompactButton(moveButton_);
    connect(moveButton_, &QPushButton::clicked, this, &DictionaryPage::moveRules);
    addTileAction(rulesTile_, moveButton_);
    rulesTile_->hide();
    layout->addWidget(rulesTile_);

    libraryTile_ = new InfoTile(q(kLibraryField.title), q(kLibraryField.description),
                                ui::designIcon(QStringLiteral("book-blue.svg")));
    ui::setStyleProperty(libraryTile_, ui::prop::tileId, QStringLiteral("dictionaryLibrary"));
    count_ = new QLabel;
    count_->setObjectName(QStringLiteral("dictionaryCount"));
    libraryTile_->addTrailing(count_);
    library_ = new DictionaryLibrary(termEdit_);
    connect(library_, &DictionaryLibrary::starToggled, this, &DictionaryPage::star);
    connect(library_, &DictionaryLibrary::editRequested, this, &DictionaryPage::beginEdit);
    connect(library_, &DictionaryLibrary::removeRequested, this, &DictionaryPage::remove);
    libraryTile_->addBody(library_);
    addTileGroup(QString(), {libraryTile_}, 1);

    steerTile_ = new SettingTile(q(kSteerField.title), q(kSteerField.description),
                                 ui::designIcon(QStringLiteral("microphone-blue.svg")));
    ui::setStyleProperty(steerTile_, ui::prop::tileId, QStringLiteral("dictionarySteer"));
    connect(steerTile_->checkbox(), &QAbstractButton::toggled, this, &DictionaryPage::setSteering);
    learnTile_ = new SettingTile(q(kLearnField.title), q(kLearnField.description),
                                 ui::designIcon(QStringLiteral("wand-purple.svg")));
    ui::setStyleProperty(learnTile_, ui::prop::tileId, QStringLiteral("dictionaryLearn"));
    connect(learnTile_->checkbox(), &QAbstractButton::toggled, this, &DictionaryPage::setLearning);
    learnGateTile_ = new InfoTile(q(kLearnField.title), q(kLearnGateLine),
                                  ui::designIcon(QStringLiteral("wand-purple.svg")));
    ui::setStyleProperty(learnGateTile_, ui::prop::tileId, QStringLiteral("dictionaryLearnGate"));
    // Named like the page it opens, short enough for a narrow tiled window.
    learnTurnOn_ = neutralButton(new Button(QStringLiteral("Apps && styles…")));
    learnTurnOn_->setObjectName(QStringLiteral("dictionaryLearnTurnOnButton"));
    learnTurnOn_->setToolTip(
        QStringLiteral("Open Apps & styles to turn on Read text near the cursor"));
    fitCompactButton(learnTurnOn_);
    connect(learnTurnOn_, &QPushButton::clicked, this, &DictionaryPage::openTextReading);
    addTileAction(learnGateTile_, learnTurnOn_);
    learnGateTile_->hide();
    addTileGroup(QStringLiteral("How your dictionary is used"), {steerTile_}, 1);
    addTileGroup(QString(), {learnTile_, learnGateTile_}, 1);
    // OpenWhisper's page column ends in a stretch (ensure_page), which takes any height the page
    // is given beyond what it needs.
    layout->addStretch();

    const std::vector<QWidget*> controls{
        composer_,      termEdit_,
        heardEdit_,     addButton_,
        cancelButton_,  message_,
        newTile_,       rulesTile_,
        libraryTile_,   library_,
        steerTile_,     steerTile_->checkbox(),
        learnTile_,     learnTile_->checkbox(),
        learnGateTile_, cleanupGate_,
    };
    for (std::size_t index = 0; index < controls.size(); ++index) {
        exposeControl(controlNames()[index], controls[index]);
    }
}

DictionaryPage::~DictionaryPage() = default;

void DictionaryPage::setBackendProvider(std::function<BackendInfo()> provider) {
    backendProvider() = std::move(provider);
}

text::EngineInfo DictionaryPage::engineInfo(const Json& settings) {
    const std::string engine = keys::selected_model.read(orEmpty(settings));
    std::optional<text::dictionary_page::Endpoint> endpoint;
    if (engine == "api") {
        const speech::SpeechEndpoint selected = speech::selected_endpoint(orEmpty(settings));
        endpoint = text::dictionary_page::Endpoint{selected.name, selected.is_local()};
    }
    std::optional<text::dictionary_page::RemoteBackend> remote;
    if (engine == "remote") {
        const std::optional<BackendInfo> backend = currentBackend();
        if (backend && backend->remote && backend->available) {
            remote = text::dictionary_page::RemoteBackend{backend->support, backend->name,
                                                          backend->hostName};
        }
    }
    return text::dictionary_page::engine_info(settings, endpoint, remote);
}

QString DictionaryPage::homeDetail(const Json& settings) {
    return qt::toQString(text::dictionary_page::home_detail(settings, engineInfo(settings)));
}

void DictionaryPage::openComposer(SettingsDialog& dialog) {
    dialog.selectDestination(qt::toQString(destinations::dictionary));
    dialog.reveal(dialog.control("dictionary_term_edit"));
}

SettingsSwitch* DictionaryPage::steerSwitch() const {
    return steerTile_->checkbox();
}

SettingsSwitch* DictionaryPage::learnSwitch() const {
    return learnTile_->checkbox();
}

void DictionaryPage::refreshPage() {
    showSettings(host().settings());
}

void DictionaryPage::load(const Json& settings) {
    showSettings(settings);
}

void DictionaryPage::showSettings(const Json& settings) {
    const LoadingScope scope(*this);
    settings_ = orEmpty(settings);
    const text::EngineInfo engine = engineInfo(settings_);
    const bool steering = keys::dictionary_steer_recognition.read(settings_);
    if (!engine.name.empty()) {
        cleanupGate_->setDescription(
            QStringLiteral("%1 can't listen for your words, so only Sounds like spellings get "
                           "fixed. AI cleanup uses your words too.")
                .arg(qt::toQString(engine.name)));
    }
    gates_.refresh(host().settings());
    const std::vector<text::DictionaryTerm> terms = text::load_dictionary(settings_);
    library_->setTerms(terms);
    count_->setText(QStringLiteral("%1 / %2")
                        .arg(static_cast<qulonglong>(terms.size()))
                        .arg(static_cast<qulonglong>(text::MAX_TERMS)));
    count_->setVisible(!terms.empty());
    const auto fresh = std::ranges::count_if(terms, [](const auto& term) { return term.is_new; });
    newTile_->setDescription(QStringLiteral("%1 new word%2. Undo any you don't want.")
                                 .arg(static_cast<qlonglong>(fresh))
                                 .arg(fresh == 1 ? QString() : QStringLiteral("s")));
    newTile_->setVisible(fresh != 0);
    const std::vector<std::string> moving = text::spelling_rules(settings_);
    const std::size_t rules = moving.size();
    // A moved word works without AI cleanup only through the speech model or its Sounds like
    // spelling.
    const bool works = (engine.support == text::SUPPORT_MODEL && steering) ||
                       std::ranges::all_of(moving, [](const std::string& rule) {
                           const auto taught = text::spelling_rule(rule);
                           return taught.has_value() && !taught->second.empty();
                       });
    QString reason;
    if (works) {
        reason = rules != 1
                     ? QStringLiteral("As dictionary words they also work when AI cleanup is off.")
                     : QStringLiteral("As a dictionary word it also works when AI cleanup is off.");
    } else {
        reason = rules != 1 ? QStringLiteral("They can live here with your other words.")
                            : QStringLiteral("It can live here with your other words.");
    }
    rulesTile_->setDescription(
        rules != 1
            ? QStringLiteral("%1 of your Learned rules only spell a word. %2")
                  .arg(static_cast<qulonglong>(rules))
                  .arg(reason)
            : QStringLiteral("One of your Learned rules only spells a word. %1").arg(reason));
    moveButton_->setText(rules != 1 ? QStringLiteral("Move them here")
                                    : QStringLiteral("Move it here"));
    rulesTile_->setVisible(rules != 0);
    steerSwitch()->setChecked(steering);
    // The saved choice stays for engines that take hints.
    const bool blocked = engine.support == text::SUPPORT_AFTER;
    steerTile_->setEnabled(!blocked);
    if (steering || blocked) {
        steerTile_->setDescription(
            qt::toQString(text::dictionary_page::engine_line(settings_, engine)));
    } else {
        steerTile_->setDescription(cleanupOn(settings_) ? q(kSteerOffLine)
                                                        : q(kSteerOffNoCleanupLine));
    }
    const bool reading = keys::app_context_read_text.read(settings_);
    learnSwitch()->setChecked(keys::dictionary_learn_enabled.read(settings_));
    learnTile_->setVisible(reading);
    learnGateTile_->setVisible(!reading);
    if (!editing_.empty() &&
        std::ranges::none_of(terms, [this](const auto& term) { return term.id == editing_; })) {
        cancelEdit();
    }
}

void DictionaryPage::reload() {
    showSettings(host().settings());
    host().notifyChanged(QStringLiteral("dictionary"));
}

void DictionaryPage::say(const QString& text, bool error) {
    message_->setText(text);
    ui::setStyleProperty(message_, ui::prop::tone,
                         error ? QStringLiteral("error") : QStringLiteral("ok"));
    message_->setVisible(!text.isEmpty());
}

bool DictionaryPage::mutate(const std::function<void(Json&)>& change, const QString& failure) {
    QString refusal;
    if (host().mutate(change, &refusal)) {
        return true;
    }
    say(refusal.isEmpty() ? failure : refusal, true);
    return false;
}

void DictionaryPage::submit() {
    const std::string term = qt::toUtf8(termEdit_->text());
    const std::vector<std::string> heard = text::parse_heard(qt::toUtf8(heardEdit_->text()));
    const std::string editing = editing_;
    std::optional<text::DictionaryTerm> saved;
    bool ok = false;
    if (!editing.empty()) {
        ok = mutate(
            [&](Json& settings) { saved = text::update_term(settings, editing, term, heard); },
            QStringLiteral("Couldn't save the change. Try again."));
    } else {
        ok = mutate([&](Json& settings) { saved = text::add_term(settings, term, heard); },
                    QStringLiteral("Couldn't add the word. Try again."));
    }
    if (!ok || !saved.has_value()) {
        return;
    }
    endEdit();
    termEdit_->clear();
    heardEdit_->clear();
    termEdit_->setFocus();
    reload();
    QString message = (editing.empty() ? QStringLiteral("Added %1.") : QStringLiteral("Saved %1."))
                          .arg(curlyQuoted(saved->term));
    if (saved->heard.empty() &&
        text::dictionary_page::sounds_like_only(settings_, engineInfo(settings_))) {
        message += QStringLiteral(" ") + q(kNotFixedHint);
    }
    say(message);
}

void DictionaryPage::beginEdit(const QString& termId) {
    const std::string id = qt::toUtf8(termId);
    const std::vector<text::DictionaryTerm> terms = text::load_dictionary(settings_);
    const auto found =
        std::ranges::find_if(terms, [&id](const auto& term) { return term.id == id; });
    if (found == terms.end()) {
        return;
    }
    editing_ = id;
    composer_->titleLabel()->setText(QStringLiteral("Edit a word"));
    termEdit_->setText(qt::toQString(found->term));
    std::string heard;
    for (const std::string& variant : found->heard) {
        heard += (heard.empty() ? "" : ", ") + variant;
    }
    heardEdit_->setText(qt::toQString(heard));
    addButton_->setText(QStringLiteral("Save"));
    cancelButton_->show();
    say(QString());
    host().reveal(termEdit_);
}

void DictionaryPage::cancelEdit() {
    endEdit();
    termEdit_->clear();
    heardEdit_->clear();
    say(QString());
}

void DictionaryPage::endEdit() {
    editing_.clear();
    composer_->titleLabel()->setText(q(kComposerField.title));
    addButton_->setText(QStringLiteral("Add"));
    cancelButton_->hide();
}

void DictionaryPage::star(const QString& termId, bool starred) {
    const std::string id = qt::toUtf8(termId);
    mutate([&](Json& settings) { text::set_starred(settings, id, starred); },
           QStringLiteral("Couldn't save the star. Try again."));
    reload();
}

void DictionaryPage::remove(const QString& termId) {
    const std::string id = qt::toUtf8(termId);
    const DictionaryRow* row = library_->row(termId);
    const bool forget = row != nullptr && row->term().learned && row->term().is_new;
    std::optional<text::DictionaryTerm> removed;
    const bool ok = mutate(
        [&](Json& settings) {
            removed = forget ? text::undo_learned(settings, id) : text::delete_term(settings, id);
        },
        QStringLiteral("Couldn't remove the word. Try again."));
    if (!ok) {
        return;
    }
    if (editing_ == id) {
        cancelEdit();
    }
    reload();
    if (removed.has_value()) {
        say((forget ? QStringLiteral("Forgot %1.") : QStringLiteral("Removed %1."))
                .arg(curlyQuoted(removed->term)));
    }
}

void DictionaryPage::keepNew() {
    if (mutate([](Json& settings) { text::clear_new(settings); },
               QStringLiteral("Couldn't save that. Try again."))) {
        reload();
    }
}

void DictionaryPage::moveRules() {
    std::size_t moved = 0;
    if (!mutate([&moved](Json& settings) { moved = text::move_spelling_rules(settings); },
                QStringLiteral("Couldn't move the rules. Try again."))) {
        return;
    }
    // The Learned rules page re-reads the rules that are left whenever it is shown.
    reload();
    if (moved != 0) {
        say(QStringLiteral("Moved %1 spelling rule%2 here.")
                .arg(static_cast<qulonglong>(moved))
                .arg(moved == 1 ? QString() : QStringLiteral("s")));
    }
}

void DictionaryPage::setSteering(bool checked) {
    if (save(keys::dictionary_steer_recognition.key(), checked)) {
        reload();
    }
}

void DictionaryPage::setLearning(bool checked) {
    if (save(keys::dictionary_learn_enabled.key(), checked)) {
        reload();
    }
}

void DictionaryPage::openTextReading() {
    host().selectDestination(qt::toQString(destinations::styles));
}

void registerDictionaryPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::dictionary;
    descriptor.title = QStringLiteral("Dictionary");
    descriptor.subtitle = QStringLiteral("Names, jargon, and spellings %1 should get right every "
                                         "time.")
                              .arg(qt::toQString(identity().product));
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new DictionaryPage(host, container, context.service<CleanupGates>());
    };
    descriptor.railValue = [](const Json& settings) {
        return qt::toQString(text::dictionary_page::rail_value(settings));
    };
    for (const Field& field : {kComposerField, kLibraryField, kSteerField, kLearnField}) {
        descriptor.searchFields.push_back({field.target, q(field.title), q(field.description)});
    }
    descriptor.controls = controlNames();
    registerPersonalizePage(context, std::move(descriptor));
}

void addDictionaryHomeRows(HomePage& home) {
    auto* button = neutralButton(new Button(QStringLiteral("Add word")));
    button->setObjectName(QStringLiteral("homeDictionaryAddButton"));
    fitCompactButton(button);
    const QPointer<WrappedLabel> detail =
        home.addRow(HomeCardId::Personalize, QStringLiteral("Dictionary"), QString(), button);
    button->setAccessibleName(QStringLiteral("Add word to dictionary"));
    SettingsDialog* dialog = &home.dialog();
    QObject::connect(button, &QPushButton::clicked, &home,
                     [dialog] { DictionaryPage::openComposer(*dialog); });
    home.addRefreshHook([detail](const Json& settings) {
        if (detail != nullptr) {
            detail->setText(DictionaryPage::homeDetail(settings));
        }
    });
}

} // namespace fv
