#include "ui_settings/walkthroughs_page.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/log.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_settings/detail/mailbox.h"
#include "ui_settings/settings_binder.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "walkthrough/write_up_names.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

QIcon icon(const char* file) {
    return ui::designIcon(QString::fromLatin1(file));
}

// A combo of the values an enum lists, each shown as its label, 40 px high.
ElidingComboBox* enumCombo(enums::EnumId id, const char* objectName) {
    auto* combo = new ElidingComboBox;
    combo->setObjectName(QString::fromLatin1(objectName));
    for (const Json& value : enums::values(id)) {
        const std::optional<std::string> label = enums::label(id, value);
        combo->addItem(q(label.value_or(value.get<std::string>())), q(value.get<std::string>()));
    }
    combo->setMinimumHeight(40);
    return combo;
}

// What the agents are called, in the order Settings lists them (write_up.AGENTS).
const Json& writeUpAgents() {
    return enums::installed(enums::EnumId::MeetingAgentCore);
}

QString agentName(const std::string& agent) {
    return q(agents::agent_spec(agent).name);
}

QLabel* groupTitle(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("hotkeyGroupTitle"));
    return label;
}

// The binder's resolve over one setting.
template <class Setting> SettingsBinder::Resolve read(const Setting& setting) {
    return [&setting](const Json& settings) {
        return Json(setting.read(settings));
    };
}

} // namespace

WalkthroughsPage::WalkthroughsPage(SettingsPage& page, SettingsDialog& dialog, Ports ports)
    : QWidget(&page), dialog_(dialog), ports_(std::move(ports)),
      mailbox_(std::make_shared<detail::Mailbox<WalkthroughsPage>>(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    if (ports_.hotkeysPage) {
        hotkeys_ = ports_.hotkeysPage();
    }
    buildShortcuts(layout);
    buildCopyGroup(page, layout);
    buildDocumentGroup(page, layout);
    buildWriteUpGroup(page, layout);
    bindSettings();
}

WalkthroughsPage::~WalkthroughsPage() {
    mailbox_->close();
}

QString WalkthroughsPage::railValue(const Json& settings) {
    return keys::walkthrough_write_up.read(settings) ? QStringLiteral("AI write-up on")
                                                     : QStringLiteral("As spoken");
}

void WalkthroughsPage::buildShortcuts(QVBoxLayout* layout) {
    if (hotkeys_ == nullptr) {
        return;
    }
    layout->addWidget(groupTitle(QStringLiteral("Shortcuts")));
    for (const auto& item : standard_hotkeys::walkthroughRows()) {
        layout->addWidget(hotkeys_->shortcutRow(item.key, q(item.title), q(item.description),
                                                !item.required, kShortcutPage));
    }
}

// What a walkthrough's PDF and Markdown show besides your words and screenshots.
void WalkthroughsPage::buildCopyGroup(SettingsPage& page, QVBoxLayout* layout) {
    copyFormatCombo_ =
        enumCombo(enums::EnumId::WalkthroughCopyFormat, "walkthroughCopyFormatCombo");
    copyFormatCombo_->setAccessibleName(QStringLiteral("Walkthrough default copy"));
    copyFormatTile_ = new FieldTile(
        QStringLiteral("Default copy"),
        QStringLiteral(
            "What happens when a walkthrough finishes. PDF copy (the default) copies the file "
            "to paste or attach. Agent with Auto-paste on pastes the PDF, then instructions "
            "to carry out the work, into your focused chat; the message stays unsent. "
            "With Auto-paste off, Agent copies only instructions; attach the PDF separately "
            "or use Paste latest walkthrough for both. Markdown copy is text with a file "
            "link to each screenshot. PDF and Markdown copy puts both on the clipboard; "
            "the receiving app chooses which to use. Word copies an editable .docx file."),
        copyFormatCombo_, icon("copy-gray.svg"));
    page.addTileGroup(layout, QStringLiteral("Copy"), {copyFormatTile_});
}

void WalkthroughsPage::buildDocumentGroup(SettingsPage& page, QVBoxLayout* layout) {
    titleEdit_ = new QLineEdit;
    titleEdit_->setObjectName(QStringLiteral("walkthroughTitleEdit"));
    titleEdit_->setPlaceholderText(QStringLiteral("No title"));
    titleEdit_->setAccessibleName(QStringLiteral("Walkthrough document title"));
    titleEdit_->setMinimumHeight(40);
    titleTile_ = new FieldTile(
        QStringLiteral("Title"),
        QStringLiteral("The heading at the top of the document. Leave it empty for none."),
        titleEdit_, icon("typography-blue.svg"));

    pageSizeCombo_ = enumCombo(enums::EnumId::WalkthroughPageSize, "walkthroughPageSizeCombo");
    pageSizeTile_ = new FieldTile(
        QStringLiteral("Paper size"),
        QStringLiteral("The PDF and Word document's page size. The Markdown copy has no pages."),
        pageSizeCombo_, icon("stack-slate.svg"));

    detailsTile_ = new SettingTile(
        QStringLiteral("Date, length and screenshot count"),
        QStringLiteral("A line under the title, such as \"Tue, Oct 20, 2026, 2:32 PM · "
                       "2:16 of speech · 3 screenshots\"."),
        icon("info-blue.svg"));
    aboutTile_ = new SettingTile(
        QStringLiteral("How to read it"),
        QStringLiteral("A note saying the document is spoken notes with screenshots, "
                       "each screenshot right after the words about it. It helps an "
                       "agent that reads the document follow it."),
        icon("info-blue.svg"));
    captionsTile_ = new SettingTile(
        QStringLiteral("Screenshot captions"),
        QStringLiteral("\"Screenshot 2 · 1:12\" under each screenshot: its number and "
                       "when you took it."),
        icon("palette-purple.svg"));

    page.addTileGroup(layout, QStringLiteral("Document"),
                      {titleTile_, pageSizeTile_, detailsTile_, aboutTile_, captionsTile_}, 2,
                      QStringLiteral("How the PDF and Markdown look. Your words and screenshots "
                                     "are always included; changes apply to the next "
                                     "walkthrough."));
}

// Whether an installed agent writes a finished walkthrough up, which, and how.
void WalkthroughsPage::buildWriteUpGroup(SettingsPage& page, QVBoxLayout* layout) {
    writeUpTile_ = new SettingTile(
        QStringLiteral("Write it up with an AI agent"),
        QStringLiteral("When a walkthrough finishes, your coding agent lays out a cleaner "
                       "document from your words and screenshots, then delivers it using your "
                       "walkthrough copy settings. It uses your own plan and usually takes "
                       "under a minute. The as-spoken PDF is kept beside it."),
        icon("wand-purple.svg"));

    auto* agentCombo = new ElidingComboBox;
    agentCombo_ = agentCombo;
    agentCombo->setObjectName(QStringLiteral("walkthroughWriteUpAgentCombo"));
    for (const Json& agent : writeUpAgents()) {
        const std::string id = agent.get<std::string>();
        agentCombo->addItem(agentName(id), q(id));
    }
    agentCombo->setMinimumHeight(40);
    labelAgents();
    agentTile_ = new FieldTile(
        QStringLiteral("Agent"),
        QStringLiteral("Claude Code reads the walkthrough's PDF and can open only that "
                       "folder. Codex and OpenCode open no files: they're sent the words "
                       "and screenshots, so their model must take images."),
        agentCombo, icon("cpu-blue.svg"));

    // Plain, not a rich editor: the placeholder, the default instructions, wraps instead of
    // being clipped to one line.
    instructionsEdit_ = new QPlainTextEdit;
    instructionsEdit_->setObjectName(QStringLiteral("walkthroughWriteUpEditor"));
    instructionsEdit_->setPlaceholderText(q(walkthrough::write_up::DEFAULT_INSTRUCTIONS));
    instructionsEdit_->setMinimumHeight(96);
    instructionsEdit_->setMaximumHeight(150);
    instructionsEdit_->setAccessibleName(QStringLiteral("Write-up instructions"));
    instructionsTile_ = new FieldTile(
        QStringLiteral("Instructions"),
        QStringLiteral("What to make of your words and screenshots. While it's empty, "
                       "the box shows the default."),
        instructionsEdit_, icon("typography-blue.svg"));
    notesTile_ = new SettingTile(
        QStringLiteral("Follow notes left for the agent"),
        QStringLiteral("While you talk, speak to the agent, such as \"note for the agent: "
                       "make the text purple\" or \"for whoever writes this up, skip the "
                       "setup part\". It follows the note instead of writing it into the "
                       "document. Notes can change only what the write-up says and how it "
                       "looks."),
        icon("notes-blue.svg"));
    // Notes are for the agent, so they mean nothing without a write-up.
    notesTile_->setEnabled(false);
    connect(writeUpTile_->checkbox(), &QAbstractButton::toggled, notesTile_, &QWidget::setEnabled);
    page.addTileGroup(layout, QStringLiteral("AI write-up"),
                      {writeUpTile_, agentTile_, instructionsTile_, notesTile_}, 1,
                      QStringLiteral("Optional. Your words and screenshots stay as spoken unless "
                                     "you turn this on."));
}

void WalkthroughsPage::bindSettings() {
    SettingsBinder& bindings = dialog_.bindings();
    bindings.combo(copyFormatCombo_, std::string(keys::walkthrough_copy_format.key()),
                   read(keys::walkthrough_copy_format));
    bindings.lineEdit(titleEdit_, std::string(keys::walkthrough_title.key()),
                      read(keys::walkthrough_title));
    bindings.combo(pageSizeCombo_, std::string(keys::walkthrough_page_size.key()),
                   read(keys::walkthrough_page_size));
    bindings.checkbox(detailsTile_->checkbox(), std::string(keys::walkthrough_show_details.key()),
                      read(keys::walkthrough_show_details));
    bindings.checkbox(aboutTile_->checkbox(), std::string(keys::walkthrough_show_about.key()),
                      read(keys::walkthrough_show_about));
    bindings.checkbox(captionsTile_->checkbox(), std::string(keys::walkthrough_show_captions.key()),
                      read(keys::walkthrough_show_captions));
    bindings.checkbox(writeUpTile_->checkbox(), std::string(keys::walkthrough_write_up.key()),
                      read(keys::walkthrough_write_up));
    bindings.combo(agentCombo_, std::string(keys::walkthrough_write_up_agent.key()),
                   read(keys::walkthrough_write_up_agent));
    bindings.plainText(instructionsEdit_,
                       std::string(keys::walkthrough_write_up_instructions.key()),
                       read(keys::walkthrough_write_up_instructions));
    bindings.checkbox(notesTile_->checkbox(), std::string(keys::walkthrough_write_up_notes.key()),
                      read(keys::walkthrough_write_up_notes));
}

// Marks the write-up agents the last scan didn't find; scans once if none has run.
void WalkthroughsPage::labelAgents() {
    if (!ports_.agents) {
        return;
    }
    const std::optional<agents::ScanResult> found = ports_.agents->cached_scan();
    if (!found) {
        if (!scanStarted_) {
            scanStarted_ = true;
            startScan();
        }
        return;
    }
    for (int index = 0; index < agentCombo_->count(); ++index) {
        const std::string agent = qt::toUtf8(agentCombo_->itemData(index).toString());
        const QString name = agentName(agent);
        const auto it = found->find(agent);
        const bool present = it != found->end() && it->second.has_value();
        agentCombo_->setItemText(index, present ? name : name + q(kNotInstalled));
    }
}

void WalkthroughsPage::startScan() {
    if (ports_.worker == nullptr) {
        return;
    }
    // The worker holds the agents and the mailbox, never the page.
    ports_.worker->post([agents = ports_.agents, mailbox = mailbox_] {
        try {
            agents->scan_agents();
        } catch (const std::exception& error) {
            kLog.warning("Scanning for installed agents failed: {}", error.what());
        } catch (...) {
            kLog.warning("Scanning for installed agents failed");
        }
        mailbox->post([](WalkthroughsPage& page) { page.labelAgents(); });
    });
}

void WalkthroughsPage::loadSettings(const Json& settings) {
    (void)settings;
    // _load_settings reads the shortcuts for this page too, so its rows show them.
    if (hotkeys_ != nullptr) {
        hotkeys_->reloadShortcuts();
    }
}

void WalkthroughsPage::cancelCapture() {
    if (hotkeys_ != nullptr) {
        hotkeys_->cancelCapture();
    }
}

QWidget* WalkthroughsPage::shortcutInput(std::string_view action) const {
    return hotkeys_ != nullptr ? hotkeys_->shortcutInput(action, kShortcutPage) : nullptr;
}

QWidget* WalkthroughsPage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"walkthrough_copy_format_combo", copyFormatCombo_},
        {"walkthrough_copy_format_tile", copyFormatTile_},
        {"walkthrough_title_edit", titleEdit_},
        {"walkthrough_title_tile", titleTile_},
        {"walkthrough_page_size_combo", pageSizeCombo_},
        {"walkthrough_page_size_tile", pageSizeTile_},
        {"walkthrough_details_tile", detailsTile_},
        {"walkthrough_details_check", detailsTile_->checkbox()},
        {"walkthrough_about_tile", aboutTile_},
        {"walkthrough_about_check", aboutTile_->checkbox()},
        {"walkthrough_captions_tile", captionsTile_},
        {"walkthrough_captions_check", captionsTile_->checkbox()},
        {"walkthrough_write_up_tile", writeUpTile_},
        {"walkthrough_write_up_check", writeUpTile_->checkbox()},
        {"walkthrough_write_up_agent_combo", agentCombo_},
        {"walkthrough_write_up_agent_tile", agentTile_},
        {"walkthrough_write_up_edit", instructionsEdit_},
        {"walkthrough_write_up_instructions_tile", instructionsTile_},
        {"walkthrough_write_up_notes_tile", notesTile_},
        {"walkthrough_write_up_notes_check", notesTile_->checkbox()},
    };
    for (const auto& [control, widget] : controls) {
        if (control == name) {
            return widget;
        }
    }
    return nullptr;
}

void registerWalkthroughsPage(AppContext& context, WalkthroughsOptions options) {
    SettingsPageSpec spec;
    spec.destination = destinations::walkthroughs;
    spec.title = QStringLiteral("Walkthroughs");
    spec.subtitle = QStringLiteral("Talk while you take screenshots, then paste one document into "
                                   "your agent: its shortcuts, how the document looks, and the "
                                   "optional AI write-up.");
    AppContext* app = &context;
    spec.create = [app, options = std::move(options)](QWidget* parent) -> QWidget* {
        auto* page = qobject_cast<SettingsPage*>(parent);
        if (page == nullptr || page->dialog() == nullptr) {
            throw std::logic_error("The Walkthroughs page is built inside the Settings window.");
        }
        SettingsDialog* dialog = page->dialog();
        WalkthroughsPage::Ports ports;
        if (options.hotkeysPage) {
            ports.hotkeysPage = [provider = options.hotkeysPage, dialog] {
                return provider(*dialog);
            };
        } else {
            ports.hotkeysPage = [dialog]() -> HotkeysPage* {
                if (!dialog->isRegistered(destinations::hotkeys)) {
                    return nullptr;
                }
                dialog->ensurePage(destinations::hotkeys);
                return qobject_cast<HotkeysPage*>(dialog->pageContent(destinations::hotkeys));
            };
        }
        ports.agents = options.agents ? options.agents : agents::installed_agents_if_set();
        ports.worker = options.worker;
        if (ports.worker == nullptr) {
            try {
                ports.worker = &app->io();
            } catch (const ServiceUnavailable&) {
                kLog.debug("The Walkthroughs page has no worker to scan for agents on");
            }
        }
        return new WalkthroughsPage(*page, *dialog, std::move(ports));
    };
    spec.railValue = [](const Json& settings) {
        return WalkthroughsPage::railValue(settings);
    };
    spec.controls = {"walkthrough_copy_format_combo",    "walkthrough_copy_format_tile",
                     "walkthrough_title_edit",           "walkthrough_title_tile",
                     "walkthrough_page_size_combo",      "walkthrough_page_size_tile",
                     "walkthrough_details_tile",         "walkthrough_details_check",
                     "walkthrough_about_tile",           "walkthrough_about_check",
                     "walkthrough_captions_tile",        "walkthrough_captions_check",
                     "walkthrough_write_up_tile",        "walkthrough_write_up_check",
                     "walkthrough_write_up_agent_combo", "walkthrough_write_up_agent_tile",
                     "walkthrough_write_up_edit",        "walkthrough_write_up_instructions_tile",
                     "walkthrough_write_up_notes_tile",  "walkthrough_write_up_notes_check"};
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
