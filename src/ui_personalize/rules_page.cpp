#include "ui_personalize/rules_page.h"

#include <exception>
#include <utility>

#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_prompts.h"
#include "text/personalize_copy.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/cleanup_page.h"
#include "ui_personalize/cleanup_rules_section.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/rule_dictation.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

const QString kGateDescription =
    QStringLiteral("Learned rules only apply when AI cleanup runs, so teaching and editing them "
                   "waits until it's on.");

} // namespace

RulesPage::RulesPage(PersonalizeHost& host, SettingsPage& container, AppContext& context,
                     CleanupSettings& cleanup)
    : PersonalizePage(host, container), context_(context), cleanup_(cleanup) {
    gateTile_ = cleanup_.cleanupGateTile(kGateDescription);
    turnOnButton_ = cleanup_.gates().turnOnButton(gateTile_);
    pageLayout()->addWidget(gateTile_);

    CleanupRulesSection::Hooks hooks;
    hooks.loadSettings = [&context] {
        return *context.settings().snapshot();
    };
    hooks.settingsSnapshot = [this] {
        return this->host().settings();
    };
    hooks.cleanupEnabled = [this] {
        return cleanupEnabled();
    };
    hooks.persistRules = [this](const std::vector<std::string>& rules) {
        save(keys::transcript_cleanup_rules.key(), Json(rules));
    };
    hooks.transcriber = [&context] {
        return context.find<RuleTranscriber>();
    };
    hooks.transcribeDetail = [this] {
        return transcribeDetail();
    };
    hooks.isVisible = [this] {
        return window()->isVisible();
    };
    hooks.setEscapeOverride = [this](std::function<void()> cancel) {
        if (SettingsDialog* dialog = settingsDialogOf(this)) {
            dialog->setEscapeOverride(std::move(cancel));
        }
    };
    section_ = new CleanupRulesSection(cleanup_.services(), std::move(hooks), this);
    section_->build(*this);
    // OpenWhisper's page column ends in a stretch (ensure_page).
    pageLayout()->addStretch();

    exposeControl("cleanup_rules_gate_tile", gateTile_);
    exposeControl("cleanup_rules_turn_on_btn", turnOnButton_);
    for (const std::string& name : cleanupRulesControls()) {
        exposeControl(name, section_->control(name));
    }
    if (SettingsDialog* dialog = settingsDialogOf(&container)) {
        // Settings is reused when it reopens: a take still recording at close is discarded.
        connect(dialog, &SettingsDialog::closed, section_, &CleanupRulesSection::releaseRecorder);
        cleanup_.attach(*dialog);
    }
    cleanup_.addRulesPage(this);
    updateCleanupUi();
}

RulesPage::~RulesPage() = default;

QString RulesPage::railValue(std::size_t ruleCount) {
    return q(text::cleanup_rules_page::rule_count_label(static_cast<std::int64_t>(ruleCount)));
}

bool RulesPage::cleanupEnabled() const {
    if (SettingsDialog* dialog = settingsDialogOf(const_cast<RulesPage*>(this))) {
        if (auto* page = dynamic_cast<CleanupPage*>(dialog->pageContent(destinations::cleanup))) {
            return page->cleanupEnabled();
        }
    }
    return keys::transcript_cleanup_enabled.read(host().settings());
}

QString RulesPage::transcribeDetail() const {
    const Json settings = host().settings();
    QString summary;
    if (const SettingsPageSpec* voice = context_.settingsPages().page(destinations::voice_model);
        voice != nullptr && voice->railValue) {
        try {
            summary = voice->railValue(settings);
        } catch (const std::exception& error) {
            kLog.error_with(error, "Couldn't name the dictation engine");
        }
    }
    const HomeSources* sources = context_.find<HomeSources>();
    if (sources != nullptr && sources->voiceIsCloud && sources->voiceIsCloud(settings)) {
        // The rail names the server first: "Groq · whisper-large-v3".
        return QStringLiteral("Sent to %1").arg(summary);
    }
    if (sources != nullptr && sources->voiceIsRemote && sources->voiceIsRemote(settings) &&
        sources->voiceDetail) {
        return q(sources->voiceDetail(settings));
    }
    return QStringLiteral(u"On this computer · %1").arg(summary);
}

void RulesPage::updateCleanupUi() {
    const bool enabled = cleanupEnabled();
    section_->composerTile()->setEnabled(enabled);
    section_->libraryTile()->setEnabled(enabled);
    section_->updateControls();
}

void RulesPage::load(const Json& settings) {
    section_->load(settings);
    updateCleanupUi();
    cleanup_.refreshCleanupGates();
}

void registerRulesPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::cleanup_rules;
    descriptor.title = QStringLiteral("Learned rules");
    descriptor.subtitle = QStringLiteral("Teach %1 your preferred spellings, terminology, and "
                                         "formatting. Applied whenever AI cleanup runs.")
                              .arg(q(identity().product));
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new RulesPage(host, container, context, context.service<CleanupSettings>());
    };
    descriptor.railValue = [&context](const Json& settings) {
        const CleanupSettings* cleanup = context.find<CleanupSettings>();
        const std::size_t count = cleanup != nullptr
                                      ? cleanup->ruleCount(settings)
                                      : text::resolve_transcript_cleanup_rules(settings).size();
        return RulesPage::railValue(count);
    };
    descriptor.searchFields = {
        {"cleanup_rules_gate_tile", QStringLiteral("AI cleanup is off"), kGateDescription},
        {"cleanup_rules_composer_tile", QStringLiteral("Teach a new rule"),
         QStringLiteral("Type or dictate an instruction in your own words. Your AI cleanup model "
                        "rewrites it as a clear rule, and you review it before it's saved.")},
        {"cleanup_rules_library_tile", QStringLiteral("Your rules"),
         QStringLiteral("Select a rule or double-click it to edit. Every rule applies to AI "
                        "cleanup, and to profiles set to also apply learned rules.")},
    };
    descriptor.controls = {"cleanup_rules_gate_tile", "cleanup_rules_turn_on_btn"};
    for (const std::string& name : cleanupRulesControls()) {
        descriptor.controls.push_back(name);
    }
    registerPersonalizePage(context, std::move(descriptor));
}

} // namespace fv
