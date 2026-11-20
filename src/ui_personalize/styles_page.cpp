#include "ui_personalize/styles_page.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "focus/capture_service.h"
#include "focus/catalog.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/app_styles.h"
#include "text/personalize_copy.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/fit_row.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/removable_rows.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

namespace app_styles = text::app_styles;
namespace copy = text::styles_page;

// SEARCH_FIELDS: (control, title, description).
struct Field {
    const char* target;
    const char* title;
    const char* description;
};

constexpr const char* kAppContextCopy =
    "Notices the app you're dictating into, like Outlook or Slack, so your style can fit it. "
    "Checked on this computer; nothing is sent anywhere.";
constexpr const char* kReadTextCopy =
    "Reads a little of the text around your cursor, so dictation continues your sentence and "
    "spells names the way they're already written.";
constexpr const char* kStylesTitle = "Match tone to each app";
constexpr const char* kStylesCopy =
    "Writes in a tone that fits where you are: polished for email, relaxed for chat. A cleanup "
    "profile you pick still comes first.";
constexpr const char* kOverridesCopy =
    "Put an app or website in a different style, like Notion in Work messages.";

// _EXCLUDED_COPY names the product, so it is built from identity().
QString excludedCopy() {
    return QStringLiteral("Apps whose text is never read, not even by Command Mode. Password "
                          "fields, terminals and %1 itself are always skipped.")
        .arg(qt::toQString(identity().product));
}

constexpr std::array<Field, 9> kFields{{
    {"app_context_tile", "Know which app I'm dictating into", kAppContextCopy},
    {"read_text_tile", "Read text near the cursor", kReadTextCopy},
    {"excluded_apps_tile", "Never read from", nullptr},
    {"app_styles_tile", kStylesTitle, kStylesCopy},
    {"style_email_tile", "Email",
     "Tone for Outlook, Gmail and other mail: formal, casual, very casual"},
    {"style_work_tile", "Work messages", "Tone for Slack, Teams and other work chat"},
    {"style_personal_tile", "Personal messages",
     "Tone for WhatsApp, Messages and texts to friends"},
    {"style_other_tile", "Other", "Tone for every other app: documents, notes, code"},
    {"style_overrides_tile", "Choose apps for each style", kOverridesCopy},
}};

QString fieldDescription(const Field& field) {
    return field.description != nullptr ? QString::fromUtf8(field.description) : excludedCopy();
}

// _ICONS, in app_styles::CATEGORIES order.
constexpr std::array<const char*, 4> kCardIcons{"notes-blue.svg", "layout-grid-blue.svg",
                                                "text-plus-green.svg", "world-blue.svg"};

// EXAMPLES, in app_styles::CATEGORIES then app_styles::TONES order.
constexpr std::array<std::array<const char*, 3>, 4> kExamples{{
    {"Hi Sam, thanks for the notes. I'll send the draft by Friday.",
     "Hi Sam, thanks for the notes! I'll send the draft by Friday",
     "hi sam thanks for the notes, i'll send the draft by friday"},
    {"The build is fixed. I'll deploy it after lunch.", "Build's fixed, I'll deploy it after lunch",
     "build's fixed i'll deploy it after lunch"},
    {"Are you free for dinner on Saturday? It's my treat.",
     "Are you free for dinner Saturday? It's my treat",
     "are you free for dinner saturday? it's my treat"},
    {"Remember to water the plants before we leave.",
     "Remember to water the plants before we leave",
     "remember to water the plants before we leave"},
}};

QString q(const char* text) {
    return QString::fromUtf8(text);
}

const Json& orEmpty(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
}

std::size_t categoryIndex(std::string_view category) {
    const auto found = std::ranges::find(app_styles::CATEGORIES, category);
    if (found == app_styles::CATEGORIES.end()) {
        throw std::invalid_argument("Not an app category.");
    }
    return static_cast<std::size_t>(std::distance(app_styles::CATEGORIES.begin(), found));
}

std::size_t toneIndex(std::string_view tone) {
    const auto found = std::ranges::find(app_styles::TONES, tone);
    return found == app_styles::TONES.end()
               ? 0
               : static_cast<std::size_t>(std::distance(app_styles::TONES.begin(), found));
}

QString categoryLabel(std::string_view category) {
    for (const auto& [key, label] : copy::CATEGORY_LABELS) {
        if (key == category) {
            return qt::toQString(label);
        }
    }
    return qt::toQString(category);
}

// _small_button.
Button* smallButton(const QString& text) {
    auto* button = neutralButton(new Button(text));
    fitCompactButton(button);
    return button;
}

// _picker: sized for a name, not for the longest catalogue entry.
SearchableComboBox* picker(const QString& placeholder) {
    auto* made = new SearchableComboBox;
    made->setAccessibleName(placeholder);
    made->lineEdit()->setPlaceholderText(placeholder);
    made->setMinimumContentsLength(10);
    made->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    made->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return made;
}

// _fill_picker: the list replaced only when it changed, keeping what is typed.
void fillPicker(SearchableComboBox* picker, const std::vector<std::string>& names) {
    QStringList wanted;
    for (const std::string& name : names) {
        wanted << qt::toQString(name);
    }
    QStringList shown;
    for (int index = 0; index < picker->count(); ++index) {
        shown << picker->itemText(index);
    }
    if (shown == wanted) {
        return;
    }
    const QString text = picker->currentText();
    const QSignalBlocker blocker(picker);
    picker->clear();
    picker->addItems(wanted);
    picker->setEditText(text);
}

StylesPage::Platform& platform() {
    static StylesPage::Platform current;
    return current;
}

} // namespace

void StylesPage::setPlatform(Platform replacement) {
    platform() = std::move(replacement);
}

bool StylesPage::textReadingSupported() {
    const auto& answer = platform().textReadingSupported;
    return answer ? answer() : focus::text_reading_supported();
}

std::vector<std::string> StylesPage::choices() {
    const auto& recent = platform().recentApps;
    const std::vector<focus::AppIdentity> seen = recent ? recent() : focus::recent_apps();
    std::vector<std::string> names;
    std::set<std::string> taken;
    for (const focus::AppIdentity& identity : seen) {
        const auto kind = focus::catalog::classify(identity);
        const std::string name = kind.has_value() ? kind->name : identity.name;
        if (!name.empty() && taken.insert(py::casefold(name)).second) {
            names.push_back(name);
        }
    }
    for (const std::string& name : focus::catalog::known_names()) {
        if (taken.insert(py::casefold(name)).second) {
            names.push_back(name);
        }
    }
    return names;
}

QString StylesPage::privacyNote(const Json& settings) {
    const Json& values = orEmpty(settings);
    const std::string id = llm::resolve_transcript_cleanup_provider(values);
    text::ProviderInfo provider;
    provider.name = llm::profile_display_name(id, values);
    const auto profile = llm::get_profile(id, values);
    provider.local = profile.has_value() && profile->is_local();
    return qt::toQString(copy::privacy_note(values, textReadingSupported(), provider));
}

QString StylesPage::example(std::string_view category, std::string_view tone) {
    return q(kExamples[categoryIndex(category)][toneIndex(tone)]);
}

StylesPage::StylesPage(PersonalizeHost& host, SettingsPage& container, CleanupGates& gates)
    : PersonalizePage(host, container), gates_(gates) {
    QVBoxLayout* layout = pageLayout();
    const Field& appContext = kFields[0];
    const Field& readText = kFields[1];
    const Field& excludedField = kFields[2];

    appContextTile_ = new SettingTile(q(appContext.title), fieldDescription(appContext),
                                      ui::designIcon(QStringLiteral("layout-grid-blue.svg")));
    connect(appContextCheck(), &QAbstractButton::toggled, this,
            [this](bool on) { saveSwitch(keys::app_context_enabled.key(), on); });

    readTextTile_ = new SettingTile(q(readText.title), fieldDescription(readText),
                                    ui::designIcon(QStringLiteral("typography-blue.svg")));
    connect(readTextCheck(), &QAbstractButton::toggled, this,
            [this](bool on) { saveSwitch(keys::app_context_read_text.key(), on); });
    privacy_ = settingsCaption(QString());
    readTextTile_->addBody(privacy_);

    excludedAppsTile_ = new InfoTile(q(excludedField.title), fieldDescription(excludedField),
                                     ui::designIcon(QStringLiteral("info-blue.svg")));
    excludedPicker_ = picker(QStringLiteral("App to skip"));
    excludedRows_ = new RemovableRows(
        QStringLiteral("No apps excluded."), [this](const QString& name) { removeExcluded(name); },
        excludedPicker_);
    excludedAppsTile_->addBody(excludedRows_);
    excludedAdd_ = smallButton(QStringLiteral("Add"));
    excludedAdd_->setToolTip(QStringLiteral("Never read text from this app"));
    connect(excludedAdd_, &QPushButton::clicked, this, &StylesPage::addExcluded);
    connect(excludedPicker_->lineEdit(), &QLineEdit::returnPressed, this, &StylesPage::addExcluded);
    excludedAppsTile_->addBody(new FitRow({excludedPicker_, excludedAdd_}));

    addTileGroup(QStringLiteral("App awareness"), {appContextTile_, readTextTile_});
    addTileGroup(QString(), {excludedAppsTile_}, 1);

    // Under the Styles heading rather than atop the page: app awareness works without AI cleanup.
    stylesCleanupGateTile_ = gates_.gateTile(
        QStringLiteral("Styles change how AI cleanup writes, so they only apply while it's on."));
    // After the gates' own handler has turned cleanup on.
    connect(gates_.turnOnButton(stylesCleanupGateTile_), &QPushButton::clicked, this,
            [this] { refreshPage(); });
    stylesGateTile_ = new InfoTile(
        QStringLiteral("Styles need app awareness"),
        QStringLiteral("Turn on Know which app I'm dictating into so your style can fit each app."),
        ui::designIcon(QStringLiteral("info-warning.svg")));
    ui::setStyleProperty(stylesGateTile_, ui::prop::kind, QStringLiteral("notice"));
    ui::setStyleProperty(stylesGateTile_, ui::prop::tileId, QStringLiteral("stylesGate"));
    gateButton_ = smallButton(QStringLiteral("Turn on"));
    connect(gateButton_, &QPushButton::clicked, this,
            [this] { appContextCheck()->setChecked(true); });
    // Under the text rather than beside it, so it fits a narrow window.
    auto* gateRow = new QHBoxLayout;
    gateRow->setContentsMargins(0, 0, 0, 0);
    gateRow->addWidget(gateButton_);
    gateRow->addStretch(1);
    stylesGateTile_->addBodyLayout(gateRow);

    const Field& stylesField = kFields[3];
    appStylesTile_ = new SettingTile(q(stylesField.title), fieldDescription(stylesField),
                                     ui::designIcon(QStringLiteral("palette-purple.svg")));
    connect(appStylesCheck(), &QAbstractButton::toggled, this,
            [this](bool on) { saveSwitch(keys::app_styles_enabled.key(), on); });
    addTileGroup(QStringLiteral("Styles"), {stylesCleanupGateTile_, stylesGateTile_}, 1);
    addTileGroup(QString(), {appStylesTile_}, 1);

    std::vector<std::pair<QString, QString>> toneItems;
    for (const auto& [tone, label] : copy::TONE_LABELS) {
        (void)tone;
        toneItems.emplace_back(qt::toQString(label), QString());
    }
    std::vector<QWidget*> cardTiles;
    for (std::size_t index = 0; index < app_styles::CATEGORIES.size(); ++index) {
        StyleCard& made = cards_[index];
        made.category = std::string(app_styles::CATEGORIES[index]);
        const QString label = categoryLabel(made.category);
        made.tile = new InfoTile(label, QString(), ui::designIcon(q(kCardIcons[index])));
        ui::setStyleProperty(made.tile, ui::prop::tileId,
                             QStringLiteral("style_%1").arg(qt::toQString(made.category)));
        made.bar = new SegmentedBar(toneItems, true);
        made.bar->setAccessibleName(QStringLiteral("%1 tone").arg(label));
        connect(made.bar, &SegmentedBar::activated, this,
                [this, index](int tone) { picked(index, tone); });
        made.example = new WrappedLabel;
        made.example->setObjectName(QStringLiteral("appStyleExample"));
        // Cards in a row share its height; the spare room goes below the example instead of
        // between the bar's buttons.
        auto* body = new QWidget;
        body->setObjectName(QStringLiteral("appStyleCardBody"));
        auto* column = new QVBoxLayout(body);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(8);
        column->addWidget(made.bar);
        column->addWidget(made.example);
        column->addStretch(1);
        made.tile->addBody(body);
        cardTiles.push_back(made.tile);
    }
    addTileGroup(QString(), cardTiles);

    const Field& overridesField = kFields[8];
    styleOverridesTile_ = new InfoTile(q(overridesField.title), fieldDescription(overridesField),
                                       ui::designIcon(QStringLiteral("wand-purple.svg")));
    overridePicker_ = picker(QStringLiteral("App or website"));
    overrideRows_ = new RemovableRows(
        QStringLiteral("No apps moved yet."),
        [this](const QString& match) { removeOverride(match); }, overridePicker_);
    styleOverridesTile_->addBody(overrideRows_);
    overrideCategory_ = new ElidingComboBox;
    overrideCategory_->setAccessibleName(QStringLiteral("Style for this app"));
    for (const std::string_view category : app_styles::CATEGORIES) {
        overrideCategory_->addItem(categoryLabel(category), qt::toQString(category));
    }
    overrideAdd_ = smallButton(QStringLiteral("Add"));
    overrideAdd_->setToolTip(QStringLiteral("Use this style for the app"));
    connect(overrideAdd_, &QPushButton::clicked, this, &StylesPage::addOverride);
    connect(overridePicker_->lineEdit(), &QLineEdit::returnPressed, this, &StylesPage::addOverride);
    styleOverridesTile_->addBody(new FitRow({overridePicker_, overrideCategory_, overrideAdd_}));
    addTileGroup(QString(), {styleOverridesTile_}, 1);
    // OpenWhisper's page column ends in a stretch (ensure_page).
    layout->addStretch();

    // AI cleanup changes on its own page and through the gates while this one is shown; the
    // window says so, as OpenWhisper refreshes this page whenever cleanup changes.
    if (SettingsDialog* dialog = container.dialog()) {
        connect(dialog, &SettingsDialog::settingsChanged, this, [this](const QString& kind) {
            if (kind == QStringLiteral("cleanup")) {
                refreshPage();
            }
        });
    }

    exposeControl("app_context_tile", appContextTile_);
    exposeControl("read_text_tile", readTextTile_);
    exposeControl("excluded_apps_tile", excludedAppsTile_);
    exposeControl("app_styles_tile", appStylesTile_);
    for (const StyleCard& made : cards_) {
        exposeControl("style_" + made.category + "_tile", made.tile);
    }
    exposeControl("style_overrides_tile", styleOverridesTile_);
    exposeControl("app_context_check", appContextCheck());
    exposeControl("read_text_check", readTextCheck());
    exposeControl("app_styles_check", appStylesCheck());
    exposeControl("styles_cleanup_gate_tile", stylesCleanupGateTile_);
    exposeControl("styles_gate_tile", stylesGateTile_);
}

StylesPage::~StylesPage() = default;

SettingsSwitch* StylesPage::appContextCheck() const {
    return appContextTile_->checkbox();
}

SettingsSwitch* StylesPage::readTextCheck() const {
    return readTextTile_->checkbox();
}

SettingsSwitch* StylesPage::appStylesCheck() const {
    return appStylesTile_->checkbox();
}

const StylesPage::StyleCard& StylesPage::card(std::string_view category) const {
    return cards_[categoryIndex(category)];
}

void StylesPage::load(const Json& settings) {
    show(settings);
}

void StylesPage::sync(const Json& settings) {
    show(settings);
}

void StylesPage::refreshPage() {
    show(host().settings());
}

void StylesPage::show(const Json& settings) {
    const LoadingScope scope(*this);
    const Json& values = orEmpty(settings);
    const bool awareness = keys::app_context_enabled.read(values);
    const bool readText = keys::app_context_read_text.read(values);
    const bool supported = textReadingSupported();
    appContextCheck()->setChecked(awareness);
    readTextCheck()->setChecked(readText);
    appStylesCheck()->setChecked(keys::app_styles_enabled.read(values));
    readTextTile_->setEnabled(awareness && supported);
    privacy_->setText(privacyNote(values));
    // The list also keeps Command Mode and transforms out of these apps, so it stays editable
    // whenever the app can be known.
    excludedAppsTile_->setEnabled(awareness);
    RemovableRows::Entries excluded;
    for (const std::string& name : copy::excluded(values)) {
        excluded.emplace_back(qt::toQString(name), qt::toQString(name));
    }
    excludedRows_->setRows(excluded);

    // The shared cleanup gate shows itself; this one waits behind it.
    gates_.refresh(values);
    const bool cleanup = copy::cleanup_on(values);
    stylesGateTile_->setVisible(cleanup && !awareness);
    const bool locked = !cleanup || !awareness;
    appStylesTile_->setEnabled(!locked);
    const bool active = !locked && appStylesCheck()->isChecked();
    const Json tones = app_styles::resolve_tones(values);
    const auto overrides = app_styles::resolve_overrides(values);
    for (StyleCard& made : cards_) {
        const std::string tone = tones.value(made.category, std::string(app_styles::FORMAL));
        made.bar->setCurrentIndex(static_cast<int>(toneIndex(tone)));
        made.example->setText(example(made.category, tone));
        made.tile->setDescription(qt::toQString(copy::apps_summary(made.category, overrides)));
        made.tile->setEnabled(active);
    }
    styleOverridesTile_->setEnabled(active);
    RemovableRows::Entries moved;
    for (const auto& [match, category] : overrides) {
        moved.emplace_back(
            qt::toQString(match),
            QStringLiteral("%1  →  %2").arg(qt::toQString(match)).arg(categoryLabel(category)));
    }
    overrideRows_->setRows(moved);
    const std::vector<std::string> names = choices();
    fillPicker(excludedPicker_, names);
    fillPicker(overridePicker_, names);
}

void StylesPage::saved(std::string_view key, const Json& value) {
    if (host().persist(key, value)) {
        host().notifyChanged(QStringLiteral("styles"));
    }
    show(host().settings());
}

void StylesPage::saveSwitch(std::string_view key, bool on) {
    if (!loading()) {
        saved(key, on);
    }
}

void StylesPage::picked(std::size_t card, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= app_styles::TONES.size()) {
        return;
    }
    const std::string_view tone = app_styles::TONES[static_cast<std::size_t>(index)];
    cards_[card].example->setText(example(cards_[card].category, tone));
    saveTone(cards_[card].category, tone);
}

void StylesPage::saveTone(std::string_view category, std::string_view tone) {
    Json settings = orEmpty(host().settings());
    app_styles::set_tone(settings, category, tone);
    saved(keys::app_style_tones.key(), settings[std::string(keys::app_style_tones.key())]);
}

void StylesPage::addExcluded() {
    const std::string name = py::slice(py::strip(qt::toUtf8(excludedPicker_->currentText())), 0,
                                       static_cast<std::int64_t>(focus::catalog::MAX_MATCH_CHARS));
    if (name.empty()) {
        excludedPicker_->setFocus();
        return;
    }
    std::vector<std::string> values = copy::excluded(host().settings());
    const std::string folded = py::casefold(name);
    if (std::ranges::none_of(values, [&folded](const std::string& value) {
            return py::casefold(value) == folded;
        })) {
        values.push_back(name);
    }
    excludedPicker_->setEditText(QString());
    const std::size_t keep = std::min(values.size(), kMaxExcluded);
    Json kept = Json::array();
    for (auto it = values.end() - static_cast<std::ptrdiff_t>(keep); it != values.end(); ++it) {
        kept.push_back(*it);
    }
    saved(keys::app_context_excluded_apps.key(), kept);
}

void StylesPage::removeExcluded(const QString& name) {
    const std::string folded = py::casefold(qt::toUtf8(name));
    Json kept = Json::array();
    for (const std::string& value : copy::excluded(host().settings())) {
        if (py::casefold(value) != folded) {
            kept.push_back(value);
        }
    }
    saved(keys::app_context_excluded_apps.key(), kept);
}

void StylesPage::addOverride() {
    const std::string name = py::strip(qt::toUtf8(overridePicker_->currentText()));
    if (name.empty()) {
        overridePicker_->setFocus();
        return;
    }
    Json settings = orEmpty(host().settings());
    app_styles::set_override(settings, name,
                             qt::toUtf8(overrideCategory_->currentData().toString()));
    overridePicker_->setEditText(QString());
    saved(keys::app_style_overrides.key(), settings[std::string(keys::app_style_overrides.key())]);
}

void StylesPage::removeOverride(const QString& match) {
    Json settings = orEmpty(host().settings());
    app_styles::remove_override(settings, qt::toUtf8(match));
    saved(keys::app_style_overrides.key(), settings[std::string(keys::app_style_overrides.key())]);
}

void registerStylesPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::styles;
    descriptor.title = QStringLiteral("Apps & styles");
    descriptor.subtitle =
        QStringLiteral("Know which app you're dictating into, and write in a tone that fits it.");
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new StylesPage(host, container, context.service<CleanupGates>());
    };
    descriptor.railValue = [](const Json& settings) {
        return qt::toQString(copy::rail_value(settings));
    };
    for (const Field& field : kFields) {
        descriptor.searchFields.push_back({field.target, q(field.title), fieldDescription(field)});
    }
    for (const Field& field : kFields) {
        descriptor.controls.emplace_back(field.target);
    }
    for (const char* name : {"app_context_check", "read_text_check", "app_styles_check",
                             "styles_cleanup_gate_tile", "styles_gate_tile"}) {
        descriptor.controls.emplace_back(name);
    }
    registerPersonalizePage(context, std::move(descriptor));
}

void addStylesHomeRows(HomePage& home) {
    auto* toggle = new SettingsSwitch;
    const QPointer<WrappedLabel> detail = home.addRow(
        HomeCardId::Personalize, q(kStylesTitle), QString(), toggle,
        std::string(destinations::styles), "app_styles_check",
        [](const Json& settings) {
            return QVariant(keys::app_styles_enabled.read(orEmpty(settings)));
        },
        std::string(keys::app_styles_enabled.key()));
    const QPointer<SettingsSwitch> control = toggle;
    home.addRefreshHook([detail, control](const Json& settings) {
        const QString reason = qt::toQString(copy::styles_blocker(settings));
        if (control != nullptr) {
            control->setEnabled(reason.isEmpty());
            control->setToolTip(reason);
        }
        if (detail != nullptr) {
            detail->setText(reason.isEmpty() ? qt::toQString(copy::home_copy(settings)) : reason);
        }
    });
}

} // namespace fv
