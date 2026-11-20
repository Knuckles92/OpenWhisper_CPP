#include "ui_downloads/voice_model_page.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/log.h"
#include "models/speech_catalog.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/store.h"
#include "speech/api_models.h"
#include "ui_downloads/model_assignments.h"
#include "ui_settings/settings_fields.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/dictation_languages_field.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/speech_backend_picker.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_models"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// The engine whose local fields show first: the saved one when it has local fields, else the
// default engine.
QString initialLocalBackend(const Json& settings, const LocalEngineCatalog& catalog) {
    const QString saved = q(voice_engine(settings));
    return catalog.knows && catalog.knows(saved) ? saved : QStringLiteral("parakeet");
}

} // namespace

// What a worker may touch: never the page itself, which it reaches only through deliver().
struct VoiceModelPage::Mailbox {
    std::mutex mutex;
    VoiceModelPage* page = nullptr;
    qt::TokenGate gate;        // scans of what is downloaded
    qt::TokenGate runtimeGate; // runtime checks

    // From the io executor: runs fn(page) on the GUI thread if the page still lives and token is
    // still the newest of `which` when it runs.
    template <class Fn> void deliver(qt::TokenGate& which, std::uint64_t token, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (page == nullptr) {
            return;
        }
        // The page cannot be destroyed while the lock is held, so post_task's QPointer is made
        // on a live object; from then on it drops the task if the page goes.
        which.deliver(page, token,
                      [target = page, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }
};

VoiceModelPage::VoiceModelPage(SettingsStore& settings, VoiceModelPageHost& host,
                               http::Client& http, Executor& io, LocalEngineCatalog catalog,
                               VoiceModelPageOptions options, QWidget* parent)
    : QWidget(parent), settings_(settings), host_(host), http_(http), io_(io),
      catalog_(std::move(catalog)), options_(std::move(options)),
      mailbox_(std::make_shared<Mailbox>()) {
    mailbox_->page = this;
    setObjectName(QStringLiteral("voiceModelPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    groupTitle(layout, QStringLiteral("Engine"));
    auto* card = new QFrame();
    card->setObjectName(QStringLiteral("settingsTile"));
    ui::setStyleProperty(card, ui::prop::kind, QStringLiteral("field"));
    auto* inner = new QVBoxLayout(card);
    inner->setContentsMargins(16, 14, 16, 14);
    inner->setSpacing(10);
    layout->addWidget(card);

    engineCombo_ = new ElidingComboBox();
    engineCombo_->setObjectName(QStringLiteral("ondemandEngineCombo"));
    engineCombo_->setMinimumHeight(40);
    populateBackendCombo(engineCombo_, model_values());
    connect(engineCombo_, &QComboBox::currentIndexChanged, this,
            [this] { onEngineComboChanged(); });
    inner->addWidget(settingsField(QStringLiteral("Recording engine"), engineCombo_));

    caption_ = settingsCaption(QString());
    inner->addWidget(caption_);

    const std::shared_ptr<const Json> saved = settings_.snapshot();
    speechControls_ =
        new LocalEngineControls(settings_, catalog_, initialLocalBackend(*saved, catalog_), true);
    connect(speechControls_, &LocalEngineControls::engineSettingsChanged, this,
            [this] { onSpeechSettingsChanged(); });
    inner->addWidget(speechControls_);

    apiServer_ = new SpeechServerControls(settings_, http_, io_);
    if (options_.clock != nullptr) {
        apiServer_->setClock(*options_.clock);
    }
    if (options_.apiKey) {
        apiServer_->setKeyProvider(options_.apiKey);
    }
    connect(apiServer_, &SpeechServerControls::changed, this, [this] { onApiServerChanged(); });
    connect(apiServer_, &SpeechServerControls::helpRequested, this,
            [this](const QString& destination) { onApiHelp(destination); });
    inner->addWidget(apiServer_);

    languages_ = new DictationLanguagesField(settings_);
    connect(languages_, &DictationLanguagesField::changed, this, [this] { onLanguagesChanged(); });
    inner->addWidget(settingsField(QStringLiteral("Languages I dictate in"), languages_));

    inventoryTitle_ = groupTitle(layout, QStringLiteral("On this computer"));
    inventoryRow_ = new QWidget();
    inventoryRow_->setObjectName(QStringLiteral("engineInventoryRow"));
    auto* row = new QHBoxLayout(inventoryRow_);
    row->setContentsMargins(3, 0, 0, 0);
    row->setSpacing(12);
    inventoryLabel_ = settingsCaption(QString());
    inventoryLabel_->setObjectName(QStringLiteral("engineInventoryLabel"));
    row->addWidget(inventoryLabel_, 1);
    downloadsButton_ = neutralButton(new Button(QStringLiteral("Get models and runtimes")));
    downloadsButton_->setObjectName(QStringLiteral("engineInventoryButton"));
    connect(downloadsButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT downloadsRequested(engineFilter()); });
    row->addWidget(downloadsButton_);
    layout->addWidget(inventoryRow_);

    layout->addWidget(buildFootnote());

    loadEngineAndRuntime();
    refreshEngineInventory();
}

VoiceModelPage::~VoiceModelPage() {
    // A worker holds the mailbox, never the page: from here nothing is delivered.
    const std::lock_guard lock(mailbox_->mutex);
    mailbox_->page = nullptr;
}

QFrame* VoiceModelPage::buildFootnote() {
    auto* card = new QFrame();
    card->setObjectName(QStringLiteral("textModelFootnoteCard"));
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(14, 9, 14, 9);
    row->setSpacing(12);
    auto* icon = new QLabel();
    icon->setObjectName(QStringLiteral("textModelFootnoteIcon"));
    icon->setFixedSize(18, 18);
    icon->setPixmap(ui::designIcon(QStringLiteral("info-blue.svg")).pixmap(16, 16));
    row->addWidget(icon, 0, Qt::AlignTop);
    footnote_ = new WrappedLabel(q(kVoiceFootnote));
    footnote_->setObjectName(QStringLiteral("textModelFootnote"));
    row->addWidget(footnote_, 1);
    return card;
}

QString VoiceModelPage::railValue(const Json& settings) {
    return q(voice_summary(settings));
}

QString VoiceModelPage::currentEngine() const {
    return engineCombo_->currentData().toString();
}

QString VoiceModelPage::engineFilter() const {
    return q(::fv::engine_filter(currentEngine().toStdString()));
}

const std::vector<std::string>& VoiceModelPage::controlNames() {
    static const std::vector<std::string> names{"engine_combo",           "engine_caption",
                                                "speech_controls",        "api_server",
                                                "engine_inventory_title", "engine_inventory_row",
                                                "engine_inventory_label", "speech_download_button"};
    return names;
}

QWidget* VoiceModelPage::namedControl(std::string_view name) {
    if (name == "engine_combo") {
        return engineCombo_;
    }
    if (name == "engine_caption") {
        return caption_;
    }
    if (name == "speech_controls") {
        return speechControls_;
    }
    if (name == "api_server") {
        return apiServer_;
    }
    if (name == "dictation_languages") {
        return languages_;
    }
    if (name == "engine_inventory_title") {
        return inventoryTitle_;
    }
    if (name == "engine_inventory_row") {
        return inventoryRow_;
    }
    if (name == "engine_inventory_label") {
        return inventoryLabel_;
    }
    if (name == "speech_download_button") {
        return downloadsButton_;
    }
    return nullptr;
}

// --- Choosing an engine ---

void VoiceModelPage::onEngineComboChanged() {
    chooseBackend(currentEngine());
}

bool VoiceModelPage::chooseBackend(const QString& backend) {
    const std::string id = backend.toStdString();
    const auto known = model_values();
    if (std::ranges::find(known, std::string_view(id)) == known.end()) {
        return false;
    }
    const QString display = backendDisplayName(id);
    try {
        if (backendHandler_) {
            backendHandler_(display);
        } else {
            save_model_selection(settings_, id);
        }
    } catch (const std::exception& error) {
        kLog.warning("Couldn't change voice model: {}", error.what());
        host_.showMessage(
            QStringLiteral("Couldn't change voice model: %1").arg(QString::fromUtf8(error.what())));
        refreshEngineSelection();
        return false;
    }
    {
        const QSignalBlocker blocker(engineCombo_);
        engineCombo_->setCurrentIndex(std::max(0, engineCombo_->findData(backend)));
    }
    updateEngineCaption();
    updateEngineFields();
    refreshEngineInventory();
    host_.refreshRailValues();
    Q_EMIT backendChanged(display);
    return true;
}

void VoiceModelPage::onSpeechSettingsChanged() {
    refreshEngineInventory();
    Q_EMIT engineSettingsChanged();
    host_.refreshRailValues();
}

void VoiceModelPage::onApiServerChanged() {
    // The server and model are saved already; the engine follows them. OpenWhisper lets the
    // hook's exception leave the slot, which must not happen in C++ (it would unwind through Qt's
    // signal delivery), so it is shown as choose_backend shows its own.
    try {
        if (backendHandler_) {
            backendHandler_(QStringLiteral("API"));
        }
    } catch (const std::exception& error) {
        kLog.warning("Couldn't change voice model: {}", error.what());
        host_.showMessage(
            QStringLiteral("Couldn't change voice model: %1").arg(QString::fromUtf8(error.what())));
        host_.refreshRailValues();
        return;
    }
    Q_EMIT backendChanged(QStringLiteral("API"));
    host_.refreshRailValues();
}

void VoiceModelPage::onApiHelp(const QString& destination) {
    if (destination == QStringLiteral("api_keys")) {
        host_.focusApiKeys(q(apiServer_->endpoint().api_key_env));
    }
    Q_EMIT helpRequested(destination);
}

void VoiceModelPage::onLanguagesChanged() {
    speechControls_->syncLanguageField();
    host_.notifyChanged(QStringLiteral("languages"));
}

// --- Showing the saved engine ---

void VoiceModelPage::loadSettings(const Json&) {
    // The engine is read through load_model_selection, which also rewrites a pre-2.0 API name.
    loadEngineAndRuntime();
    refreshEngineInventory();
}

void VoiceModelPage::refreshEngineSelection() {
    loadEngineAndRuntime();
    refreshEngineInventory();
    host_.refreshRailValues();
}

void VoiceModelPage::loadEngineAndRuntime() {
    // A pre-2.0 API name is rewritten to the model it meant, judged by the page's clock: the
    // same date the API server's fields judge OpenAI's retirement by.
    const std::string selected =
        options_.clock != nullptr
            ? load_model_selection(settings_, speech::local_today(*options_.clock))
            : load_model_selection(settings_);
    int index = engineCombo_->findData(q(selected));
    if (index < 0) {
        index = engineCombo_->findData(q(keys::selected_model.default_value()));
    }
    {
        const QSignalBlocker blocker(engineCombo_);
        engineCombo_->setCurrentIndex(std::max(0, index));
    }
    updateEngineCaption();
    updateEngineFields();
    apiServer_->refresh();
}

void VoiceModelPage::updateEngineCaption() {
    caption_->setText(q(engine_caption(currentEngine().toStdString())));
}

void VoiceModelPage::updateEngineFields() {
    const QString backend = currentEngine();
    const bool local = catalog_.knows && catalog_.knows(backend);
    speechControls_->setVisible(local);
    downloadsButton_->setVisible(local);
    if (local) {
        speechControls_->setBackend(backend);
    }
    apiServer_->setVisible(backend == QStringLiteral("api"));
    languages_->setBackend(backend);
    // Neither the API server nor a paired computer keeps anything on this one.
    const bool onThisComputer = engine_is_local(backend.toStdString());
    inventoryTitle_->setVisible(onThisComputer);
    inventoryRow_->setVisible(onThisComputer);
}

// --- What the engine has on this computer ---

void VoiceModelPage::refresh(bool scan) {
    loadEngineAndRuntime();
    refreshEngineInventory();
    host_.refreshRailValues();
    if (scan) {
        startScan();
    }
}

void VoiceModelPage::refreshEngineInventory() {
    const std::string backend = currentEngine().toStdString();
    if (!engine_is_local(backend)) {
        inventoryLabel_->setText(QString());
        return;
    }
    if (lookupFailed_) {
        inventoryLabel_->setText(q(kInventoryFailed));
        return;
    }
    // The last shared scan stands in until this page's own answers (refresh's
    // peek_cached_models), so a new window does not start at "0 of N".
    if (!scanned_ && options_.peekCachedModels) {
        try {
            if (const auto known = options_.peekCachedModels()) {
                cached_ = std::set<std::string>(known->begin(), known->end());
            }
        } catch (const std::exception& error) {
            kLog.debug("Shared model scan unavailable: {}", error.what());
        }
    }
    try {
        const std::string device = models::selected_device(*settings_.snapshot(), backend);
        inventoryCount_ = engine_inventory_count(backend, cached_);
        const auto key = std::make_pair(backend, device);
        inventoryKey_ = key;
        const auto known = runtimeLabels_.find(key);
        inventoryLabel_->setText(q(
            inventoryCount_ + (known != runtimeLabels_.end() ? known->second : std::string("."))));
        // The runtime is looked up again on every refresh, so an install shows at once; the
        // answer is keyed by (backend, device).
        if (options_.runtimeLabel) {
            startRuntimeCheck(backend, device);
        } else {
            applyRuntimeLabel(backend, device, std::string(kRuntimeCheckFailed));
        }
    } catch (const std::exception& error) {
        kLog.debug("Engine inventory lookup failed: {}", error.what());
        inventoryLabel_->setText(q(kInventoryFailed));
    }
}

void VoiceModelPage::applyRuntimeLabel(const std::string& backend, const std::string& device,
                                       const std::string& label) {
    const auto key = std::make_pair(backend, device);
    runtimeLabels_[key] = label;
    if (currentEngine().toStdString() == backend && inventoryKey_ == key) {
        inventoryLabel_->setText(q(inventoryCount_ + label));
    }
}

void VoiceModelPage::startRuntimeCheck(const std::string& backend, const std::string& device) {
    const std::uint64_t token = mailbox_->runtimeGate.next();
    // Bound here: the worker reaches the page only through the mailbox.
    io_.post([mailbox = mailbox_, lookup = options_.runtimeLabel, backend, device, token] {
        std::string label;
        try {
            label = lookup(backend, device);
        } catch (const std::exception& error) {
            kLog.debug("Engine runtime check failed: {}", error.what());
            label = std::string(kRuntimeCheckFailed);
        }
        mailbox->deliver(mailbox->runtimeGate, token,
                         [backend, device, label = std::move(label)](VoiceModelPage& page) {
                             page.applyRuntimeLabel(backend, device, label);
                         });
    });
}

void VoiceModelPage::startScan() {
    const std::uint64_t token = mailbox_->gate.next();
    // Bound here: the worker reaches the page only through the mailbox.
    io_.post([mailbox = mailbox_, lookup = options_.cachedModels, token] {
        std::optional<std::set<std::string>> found = std::set<std::string>();
        try {
            if (lookup) {
                const std::vector<std::string> keys = lookup();
                found->insert(keys.begin(), keys.end());
            }
        } catch (const std::exception& error) {
            kLog.debug("Model scan failed: {}", error.what());
            found.reset();
        }
        mailbox->deliver(mailbox->gate, token, [found = std::move(found)](VoiceModelPage& page) {
            page.onScanFinished(found);
        });
    });
}

void VoiceModelPage::onScanFinished(const std::optional<std::set<std::string>>& found) {
    scanned_ = true;
    lookupFailed_ = !found.has_value();
    if (found) {
        cached_ = *found;
    }
    refreshEngineInventory();
    host_.refreshRailValues();
}

} // namespace fv
