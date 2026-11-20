#include "ui_settings/app_pages.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <QMessageBox>
#include <QMetaMethod>
#include <QSystemTrayIcon>

#include "appcore/mic_priority.h"
#include "appcore/rail_values.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "audio/devices.h"
#include "audio/player.h"
#include "base/cancel.h"
#include "base/desktop_session.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "credentials/dotenv.h"
#include "history/manager.h"
#include "recorder/recorder.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/endpoint_key_check.h"
#include "speech/endpoints.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_overlay/recording_look_preview.h"
#include "ui_settings/api_keys_section.h"
#include "ui_settings/appearance_page.h"
#include "ui_settings/developer_page.h"
#include "ui_settings/general_page.h"
#include "ui_settings/hotkeys_page_adapter.h"
#include "ui_settings/recording_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_window.h"

namespace fv {
namespace {

QString q(std::string_view text) {
    return qt::toQString(text);
}

// "OpenWhisper" in OpenWhisper's copy is the product here.
QString branded(const char* text) {
    return QString::fromUtf8(text).replace(QStringLiteral("{product}"), q(identity().product));
}

SettingsPage& pageOf(QWidget* parent) {
    auto* page = qobject_cast<SettingsPage*>(parent);
    if (page == nullptr || page->dialog() == nullptr) {
        throw std::logic_error("The App pages are built inside the Settings window.");
    }
    return *page;
}

template <class T> T* found(AppContext& context, T& (AppContext::*accessor)()) {
    try {
        return &(context.*accessor)();
    } catch (const ServiceUnavailable&) {
        return nullptr;
    }
}

std::vector<std::string> names(const std::vector<appcore::CredentialEntry>& entries) {
    std::vector<std::string> all;
    for (const auto& entry : entries) {
        all.push_back(entry.env_name);
    }
    return all;
}

} // namespace

bool SettingsPagesSignals::hasHotkeysReceiver() const {
    return isSignalConnected(QMetaMethod::fromSignal(&SettingsPagesSignals::hotkeysChanged));
}

AppPages::AppPages(AppContext& context)
    : context_(context), signals_(std::make_shared<SettingsPagesSignals>()) {}

AppPages::~AppPages() = default;

AppPagesHooks AppPages::resolvedHooks() {
    AppPagesHooks hooks = hooks_;
    if (!hooks.trayAvailable) {
        hooks.trayAvailable = [] {
            return QSystemTrayIcon::isSystemTrayAvailable();
        };
    }
    if (!hooks.meetingActive) {
        hooks.meetingActive = [] {
            return false;
        };
    }
    if (!hooks.playbackPlaying) {
        hooks.playbackPlaying = [] {
            return audio::player()->is_playing();
        };
    }
    audio::Backend* backend = found(context_, &AppContext::audio);
    if (!hooks.listDevices) {
        hooks.listDevices = [backend] {
            if (backend == nullptr) {
                throw std::runtime_error("No audio system is available.");
            }
            return audio::list_input_devices(*backend);
        };
    }
    if (!hooks.refreshDevices) {
        hooks.refreshDevices = [backend] {
            if (backend == nullptr) {
                throw std::runtime_error("No audio system is available.");
            }
            return backend->refresh();
        };
    }
    const Paths* paths = nullptr;
    try {
        paths = &context_.paths();
    } catch (const ServiceUnavailable&) {
    }
    if (!hooks.previewRecorder && backend != nullptr && paths != nullptr) {
        hooks.previewRecorder = [backend, paths](const Json& settings) {
            RecorderOptions options;
            options.device_priority = appcore::mic::saved_priority(settings);
            options.output_file = RecordingLookPreview::previewPath();
            return std::make_unique<Recorder>(*backend, *paths, std::move(options));
        };
    }
    if (hooks.clock == nullptr) {
        hooks.clock = found(context_, &AppContext::clock);
    }
    if (HistoryManager* history = context_.find<HistoryManager>()) {
        if (!hooks.recordingsUsage) {
            hooks.recordingsUsage = [history] {
                return history->get_recordings_usage();
            };
        }
        if (!hooks.recordingsOverLimit) {
            hooks.recordingsOverLimit = [history](std::optional<std::int64_t> count,
                                                  std::optional<std::int64_t> bytes) {
                return history->recordings_over_limit(count, bytes);
            };
        }
        if (!hooks.setRetention) {
            hooks.setRetention = [history](std::optional<std::int64_t> count,
                                           std::optional<std::int64_t> bytes) {
                history->set_retention(count, bytes);
            };
        }
    }
    if (!hooks.recordingsOverLimit) {
        // No history yet: nothing a limit could delete.
        hooks.recordingsOverLimit = [](std::optional<std::int64_t>, std::optional<std::int64_t>) {
            return std::vector<RecordingInfo>{};
        };
    }
    if (!hooks.setRetention) {
        hooks.setRetention = [](std::optional<std::int64_t>, std::optional<std::int64_t>) {
        };
    }
    if (hooks.credentials == nullptr) {
        hooks.credentials = found(context_, &AppContext::credentials);
    }
    if (hooks.env == nullptr) {
        hooks.env = &system_env();
    }
    if (hooks.dotenv == nullptr && paths != nullptr) {
        if (dotenv_ == nullptr) {
            dotenv_ = std::make_unique<DotEnv>(paths->env_file());
        }
        hooks.dotenv = dotenv_.get();
    }
    if (!hooks.verifyKey) {
        http::Client* client = found(context_, &AppContext::http);
        // An empty key is the stored one (the page sends "" for a saved key rather than read
        // the credential store on the GUI thread); this runs on the page's worker.
        hooks.verifyKey = [client, credentials = hooks.credentials, env = hooks.env,
                           dotenv = hooks.dotenv](const speech::SpeechEndpoint& endpoint,
                                                  const std::string& key) {
            if (client == nullptr) {
                throw std::runtime_error("No HTTP client is available.");
            }
            speech::KeyCheckOptions options;
            options.env = env;
            if (credentials != nullptr && env != nullptr && dotenv != nullptr) {
                options.stored_key = [&endpoint, credentials, env, dotenv] {
                    return speech::api_key(endpoint, *credentials, *env, *dotenv);
                };
            }
            const speech::KeyCheck check =
                speech::verify_endpoint_key(*client, endpoint, key, CancelToken{}, options);
            return std::pair<bool, std::string>(check.ok, check.detail);
        };
    }
    if (!hooks.dialog) {
        AppContext* context = &context_;
        hooks.dialog = [context]() -> SettingsDialog* {
            SettingsWindow* window = context->find<SettingsWindow>();
            return window != nullptr ? window->dialog() : nullptr;
        };
    }
    if (hooks.worker == nullptr) {
        hooks.worker = found(context_, &AppContext::io);
    }
    return hooks;
}

std::shared_ptr<ApiKeysState> AppPages::apiKeysState() {
    if (apiKeys_ == nullptr) {
        const AppPagesHooks hooks = resolvedHooks();
        if (hooks.credentials == nullptr || hooks.env == nullptr || hooks.dotenv == nullptr ||
            hooks.worker == nullptr) {
            return nullptr;
        }
        apiKeys_ = ApiKeysState::create(*hooks.credentials, *hooks.env, *hooks.dotenv,
                                        *hooks.worker, context_.ui());
    }
    return apiKeys_;
}

QString AppPages::hotkeysRailValue(const Json& settings) const {
    // _hotkey_rail_value: the shortcuts the Hotkeys page holds, else the saved ones.
    if (hotkeysPage_ != nullptr && !hotkeysPage_->currentHotkeys().empty()) {
        Json held = Json::object();
        held[std::string(keys::hotkeys.key())] = hotkeysPage_->currentHotkeys();
        return HotkeysPage::hotkeysRailValue(held);
    }
    return HotkeysPage::hotkeysRailValue(settings);
}

QString AppPages::apiKeysRailValue(const Json& settings) {
    const std::vector<appcore::CredentialEntry> entries = credentialEntries(settings);
    const std::shared_ptr<ApiKeysState> state = apiKeysState();
    if (state == nullptr) {
        return q(appcore::api_keys_rail_value(
            entries, [](std::string_view) { return CredentialSource::None; }));
    }
    const std::vector<std::string> wanted = names(entries);
    std::function<SettingsDialog*()> dialog = resolvedHooks().dialog;
    if (!state->knows(wanted) && !state->reading(wanted)) {
        // The store blocks: read it on the worker and redraw once it has answered.
        state->read(wanted, [dialog] {
            if (SettingsDialog* window = dialog ? dialog() : nullptr) {
                window->refreshRailValues();
            }
        });
    }
    // Python's rail asks the store, the environment and .env on every redraw; here the answers
    // are kept, so each opening of a window reads them again (a key set in the environment or
    // .env since, while the API keys page was never built).
    if (SettingsDialog* window = dialog ? dialog() : nullptr;
        window != nullptr && window != watchedDialog_) {
        watchedDialog_ = window;
        QPointer<SettingsDialog> guarded = window;
        std::weak_ptr<ApiKeysState> weak = state;
        QObject::connect(window, &SettingsDialog::windowShown, window, [guarded, weak] {
            const std::shared_ptr<ApiKeysState> alive = weak.lock();
            if (alive == nullptr || guarded == nullptr) {
                return;
            }
            alive->read(names(credentialEntries(*guarded->settingsSnapshot())), [guarded] {
                if (guarded != nullptr) {
                    guarded->refreshRailValues();
                }
            });
        });
    }
    return q(state->railValue(entries));
}

void AppPages::registerPages() {
    SettingsPageRegistry& registry = context_.settingsPages();

    {
        SettingsPageSpec spec;
        spec.destination = destinations::recording;
        spec.title = QStringLiteral("Recording");
        spec.subtitle = QStringLiteral("Microphone, saved audio retention, and the live preview.");
        spec.create = [this](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            const AppPagesHooks hooks = resolvedHooks();
            if (hooks.worker == nullptr) {
                throw std::logic_error("The Recording page needs a worker executor.");
            }
            RecordingPage::Ports ports;
            ports.microphones.listDevices = hooks.listDevices;
            ports.microphones.refreshDevices = hooks.refreshDevices;
            ports.microphones.meetingActive = hooks.meetingActive;
            ports.microphones.playbackPlaying = hooks.playbackPlaying;
            ports.microphones.worker = hooks.worker;
            ports.usage = hooks.recordingsUsage;
            ports.overLimit = hooks.recordingsOverLimit;
            ports.setRetention = hooks.setRetention;
            ports.worker = hooks.worker;
            ports.confirm = hooks.confirm;
            return new RecordingPage(page, *page.dialog(), *signals_, std::move(ports));
        };
        spec.railValue = [](const Json& settings) {
            return q(appcore::recording_rail_value(settings));
        };
        spec.searchFields = {
            {"audio_device_tile", QStringLiteral("Preferred microphone"),
             QStringLiteral("Used for dictation and meetings.")},
            {"microphone_backup_tile", QStringLiteral("If it's unplugged, use"),
             branded("{product} moves down this list, even in the middle of a dictation.")},
            {"recording_retention_tile", QStringLiteral("Keep recordings"),
             QStringLiteral("Keep a set number of recordings or cap the recordings folder size. "
                            "The oldest audio files are deleted automatically once the limit is "
                            "passed; the newest recording is always kept. Transcription history "
                            "text is kept separately.")},
            {"streaming_enabled_tile", QStringLiteral("Real-time transcription preview"),
             QStringLiteral("Shows text as you speak on the near-cursor overlay. Parakeet "
                            "transcribes short audio chunks with the loaded model. The final "
                            "transcript uses your selected model and follows the General paste "
                            "and clipboard settings. Its text size is under Appearance.")},
        };
        spec.controls = {
            "audio_device_combo",        "audio_device_tile",        "microphone_backup_tile",
            "recording_retention_combo", "recording_retention_tile", "max_recordings_label",
            "max_recordings_spinbox",    "max_recordings_mb_label",  "max_recordings_mb_spinbox",
            "recordings_usage_label",    "streaming_enabled_tile",   "streaming_enabled_check"};
        registry.addPage(std::move(spec));
    }

    {
        SettingsPageSpec spec;
        spec.destination = destinations::general;
        spec.title = QStringLiteral("General");
        spec.subtitle = QStringLiteral("How finished transcriptions leave the app and how the "
                                       "window behaves when you close it.");
        spec.create = [this](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            const AppPagesHooks hooks = resolvedHooks();
            return new GeneralPage(page, *page.dialog(), *signals_, hooks.trayAvailable(),
                                   is_native_wayland_session());
        };
        spec.railValue = [](const Json& settings) {
            return q(appcore::general_rail_value(settings));
        };
        spec.searchFields = {
            {"auto_paste_tile", QStringLiteral("Paste into the active window"), QString()},
            {"restore_clipboard_tile", QStringLiteral("Restore the clipboard after pasting"),
             QStringLiteral("Puts back what you had copied once a paste lands, including Command "
                            "Mode rewrites. Turn off to keep the pasted text on the clipboard.")},
            {"copy_clipboard_tile", QStringLiteral("Copy to the clipboard"),
             QStringLiteral("Keeps the transcript on the clipboard when auto-paste is off or "
                            "unavailable.")},
            {"minimize_tray_tile", QStringLiteral("Minimize to the system tray on close"),
             branded("Closing the window keeps {product} running in the tray, so hotkeys and "
                     "recording stay available.")},
            {"update_check_tile", QStringLiteral("Check for updates automatically"),
             branded("When {product} starts and every 4 hours while it runs, asks GitHub "
                     "(api.github.com) for the latest release. The request includes the app "
                     "version, no account or device ID. Nothing is installed without your "
                     "approval.")},
            {"update_notify_tile", QStringLiteral("Notify me about updates"),
             QStringLiteral("Shows an Update button in the title bar and the tray menu when a "
                            "newer release is found. Requires automatic checks.")},
            {"update_restart_prompt_tile", QStringLiteral("Ask me to restart after an update"),
             branded("When {product} is updated while it is running, offers to restart so the "
                     "update finishes. Turn off if you chose \"Don't show this again\" and "
                     "changed your mind.")},
            {"update_beta_tile", QStringLiteral("Get beta updates"),
             QStringLiteral("Also offers test releases a few days before everyone else gets "
                            "them. They are verified like any release but may have rough edges. "
                            "Turning this off never downgrades: you keep the beta until a newer "
                            "regular release is out.")},
        };
        spec.controls = {"auto_paste_tile",
                         "auto_paste_check",
                         "restore_clipboard_tile",
                         "restore_clipboard_check",
                         "copy_clipboard_tile",
                         "copy_clipboard_check",
                         "minimize_tray_tile",
                         "minimize_tray_check",
                         "update_check_tile",
                         "update_check_check",
                         "update_notify_tile",
                         "update_notify_check",
                         "update_restart_prompt_tile",
                         "update_restart_prompt_check",
                         "update_beta_tile",
                         "update_beta_check"};
        registry.addPage(std::move(spec));
    }

    {
        SettingsPageSpec spec;
        spec.destination = destinations::appearance;
        spec.title = QStringLiteral("Appearance");
        spec.subtitle = QStringLiteral("Theme, text size, and how the overlay near your pointer "
                                       "looks while you dictate.");
        spec.create = [this](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            const AppPagesHooks hooks = resolvedHooks();
            return new AppearancePage(page, *page.dialog(), *signals_, hooks.previewRecorder,
                                      hooks.clock);
        };
        spec.railValue = [](const Json& settings) {
            return q(appcore::appearance_rail_value(settings));
        };
        spec.searchFields = {
            {"ui_theme_tile", QStringLiteral("Theme"),
             QStringLiteral("Dark, light, or match your operating system.")},
            {"ui_font_scale_tile", QStringLiteral("Font size"),
             QStringLiteral("Text size in windows and dialogs. The live preview has its own size "
                            "below.")},
            {"recording_look_tile", QStringLiteral("Recording overlay"),
             QStringLiteral("How the overlay near your pointer looks while you speak. Every look "
                            "but Classic moves with your voice; Classic is the original "
                            "particles.")},
            {"streaming_layout_tile", QStringLiteral("Live preview layout"),
             QStringLiteral("Where the look goes once your words appear: shrunk into a header "
                            "above them, or full size under them.")},
            {"streaming_font_size_tile", QStringLiteral("Live preview text size"),
             QStringLiteral("The words that appear near your pointer as you speak, when "
                            "Real-time transcription preview is on under Recording.")},
        };
        spec.controls = {"ui_theme_combo",           "ui_theme_tile",
                         "ui_font_scale_combo",      "ui_font_scale_tile",
                         "streaming_layout_tile",    "streaming_layout_bar",
                         "streaming_font_size_tile", "streaming_font_size_spinbox",
                         "recording_look_bar",       "recording_look_tile",
                         "recording_look_preview",   "recording_parts",
                         "recording_dot_switch",     "recording_text_switch",
                         "recording_clock_switch"};
        registry.addPage(std::move(spec));
    }

    {
        SettingsPageSpec spec;
        spec.destination = destinations::hotkeys;
        spec.title = QStringLiteral("Hotkeys");
        spec.subtitle = QStringLiteral("Every global shortcut in one place, including the ones you "
                                       "give your profiles, transforms and actions.");
        spec.create = [this](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            auto* adapter = new HotkeysPageAdapter(*page.dialog(), context_.settings(), *signals_);
            HotkeysPage* created = adapter->createPage(parent);
            hotkeysPage_ = created;
            return created;
        };
        spec.railValue = [this](const Json& settings) {
            return hotkeysRailValue(settings);
        };
        spec.controls = {"_hotkey_instruction", "_hotkey_instruction_text", "record_mode_combo"};
        registry.addPage(std::move(spec));
    }

    {
        SettingsPageSpec spec;
        spec.destination = destinations::api_keys;
        spec.title = QStringLiteral("API keys");
        spec.subtitle = QStringLiteral("Credentials for cloud providers and custom endpoints. "
                                       "Saved keys live in your operating system's credential "
                                       "manager, never in the settings file.");
        spec.create = [this](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            const AppPagesHooks hooks = resolvedHooks();
            std::shared_ptr<ApiKeysState> state = apiKeysState();
            if (state == nullptr) {
                throw std::logic_error(
                    "The API keys page needs the credential store and a worker.");
            }
            auto* section =
                new ApiKeysSection(page, *page.dialog(), std::move(state), hooks.verifyKey);
            if (hooks.confirm) {
                section->setConfirmRemove(hooks.confirm);
            }
            QPointer<SettingsDialog> dialog = page.dialog();
            std::weak_ptr<SettingsPagesSignals> weak = signals_;
            QObject::connect(section, &ApiKeysSection::keysChanged, section, [dialog, weak] {
                // _on_api_keys_saved: the rail, then the app.
                if (dialog != nullptr) {
                    dialog->refreshRailValues();
                }
                if (const auto signals = weak.lock()) {
                    Q_EMIT signals->apiKeysChanged();
                }
            });
            return section;
        };
        spec.railValue = [this](const Json& settings) {
            return apiKeysRailValue(settings);
        };
        spec.searchFields = {
            {"api_key_credential_tile", QStringLiteral("Credential"),
             QStringLiteral("Choose the provider whose key you want to save or test.")},
            {"api_key_entry_tile", QStringLiteral("Add or replace a key"), QString()},
        };
        spec.controls = {"api_key_combo",         "api_key_credential_tile",
                         "api_key_status_icon",   "api_key_status",
                         "api_key_uses_caption",  "api_key_entry_tile",
                         "api_key_store_caption", "api_key_edit",
                         "api_key_show_button",   "api_key_save_button",
                         "api_key_test_button",   "api_key_remove_button"};
        registry.addPage(std::move(spec));
    }

    {
        SettingsPageSpec spec;
        spec.destination = destinations::advanced;
        spec.title = QStringLiteral("Developer options");
        spec.subtitle = QStringLiteral("Meeting re-transcription and developer tools.");
        spec.create = [this](QWidget* parent) -> QWidget* {
            SettingsPage& page = pageOf(parent);
            return new DeveloperPage(page, *page.dialog(), *signals_);
        };
        spec.railValue = [](const Json& settings) {
            return q(appcore::developer_rail_value(settings));
        };
        spec.searchFields = {
            {"meeting_redecode_coverage_guard_tile",
             QStringLiteral("Keep the live transcript if re-transcription is much shorter"),
             QStringLiteral("Reject a re-transcription with fewer than 80% of the live "
                            "transcript's words. Word count does not measure accuracy. Applies "
                            "after End and when retrying saved meetings. Off by default.")},
            {"developer_mode_tile", QStringLiteral("Developer mode"),
             QStringLiteral("Unlocks a Load demo meeting control on the Meeting Mode tab. The "
                            "demo opens the dashboard with a fake transcript so you can test "
                            "end-of-meeting cleanup and the final report without recording a "
                            "real meeting.")},
        };
        spec.controls = {"meeting_redecode_coverage_guard_tile",
                         "meeting_redecode_coverage_guard_check", "developer_mode_tile",
                         "developer_mode_check"};
        registry.addPage(std::move(spec));
    }
}

} // namespace fv
