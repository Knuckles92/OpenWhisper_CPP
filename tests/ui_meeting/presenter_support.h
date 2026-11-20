#pragma once

// A MeetingPresenter over a recording MeetingControls, in the shell's main window with History's
// sidebar, for the controller tests (ui_controller.py's meeting handlers on a stub runtime).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QTest>
#include <QTimer>

#include "appcore/activity.h"
#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "clipboard/temporary_clipboard.h"
#include "clipboard/testing/fake_backend.h"
#include "db/database.h"
#include "meeting_state/sql_repository.h"
#include "past_meetings_support.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_store.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_meeting/meeting_controls.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_meeting/meeting_presenter.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/tabbed_content.h"

namespace fvtest {

// Every call the presenter makes, as "name arg|arg".
class RecordingControls final : public fv::MeetingControls {
public:
    bool active = false;
    bool claimed = false;
    std::string transcript = "Speaker 1: hello";
    std::optional<std::pair<std::string, fv::meeting_agent::MeetingAgentRoute>> destination;
    std::function<bool(const fv::meeting::SpeakerUploadOffer&)> speakerConfirm;
    std::function<bool(bool, std::function<void(bool)>)> gate;
    std::vector<std::string> calls;

    static std::string cloudText(std::optional<bool> cloud) {
        return cloud ? (*cloud ? "true" : "false") : "none";
    }
    // The calls whose name starts with `prefix`.
    std::vector<std::string> named(const std::string& prefix) const {
        std::vector<std::string> found;
        for (const auto& call : calls) {
            if (call.starts_with(prefix)) {
                found.push_back(call);
            }
        }
        return found;
    }

    bool is_active() const override { return active; }
    bool is_claimed() const override { return claimed || active; }
    void start_meeting(std::optional<bool> cloud, std::string policy, std::string intent) override {
        calls.push_back("start_meeting " + cloudText(cloud) + "|" + policy + "|" + intent);
    }
    void start_demo_meeting(std::optional<bool> cloud, std::string policy) override {
        calls.push_back("start_demo_meeting " + cloudText(cloud) + "|" + policy);
    }
    void toggle_from_hotkey() override { calls.push_back("toggle_from_hotkey"); }
    std::optional<std::pair<std::string, fv::meeting_agent::MeetingAgentRoute>>
    live_intelligence_destination() const override {
        return destination;
    }
    void
    set_live_intelligence(const std::string& id,
                          std::optional<fv::meeting::LiveIntelligenceApproval> approval) override {
        calls.push_back("set_live_intelligence " + id + "|" + (approval ? "on" : "off"));
    }
    void set_start_intelligence_approval(fv::meeting::LiveIntelligenceApproval) override {
        calls.push_back("set_start_intelligence_approval");
    }
    void set_consent_gate(std::function<bool(bool, std::function<void(bool)>)> next) override {
        gate = std::move(next);
    }
    void pause() override { calls.push_back("pause"); }
    void resume() override { calls.push_back("resume"); }
    void end() override { calls.push_back("end"); }
    void open_dashboard() override { calls.push_back("open_dashboard"); }
    void open_report() override { calls.push_back("open_report"); }
    void copy_guest_link() override { calls.push_back("copy_guest_link"); }
    void retry_insights() override { calls.push_back("retry_insights"); }
    void retry_speakers() override { calls.push_back("retry_speakers"); }
    void retry_step(std::string step) override { calls.push_back("retry_step " + step); }
    void continue_in_background() override { calls.push_back("continue_in_background"); }
    void defer_finalization_card() override { calls.push_back("defer_finalization_card"); }
    void show_past_meeting(std::string id) override { calls.push_back("show_past_meeting " + id); }
    std::string copy_past_meeting_transcript(std::string id) override {
        // Runs on io: record nothing here, the GUI thread reads `calls`.
        (void)id;
        return transcript;
    }
    void delete_past_meeting(std::string id, bool recordings) override {
        calls.push_back("delete_past_meeting " + id + "|" + (recordings ? "audio" : "keep"));
    }
    void clear_past_meetings(bool recordings) override {
        calls.push_back(std::string("clear_past_meetings ") + (recordings ? "audio" : "keep"));
    }
    bool can_offer_speaker_upload() const override { return true; }
    void request_speaker_upload(
        std::string id,
        std::function<bool(const fv::meeting::SpeakerUploadOffer&)> confirm) override {
        calls.push_back("request_speaker_upload " + id);
        speakerConfirm = std::move(confirm);
    }
    void finalize_recovered(std::string id) override {
        calls.push_back("finalize_recovered " + id);
    }
    void discard_recovered(std::string id) override { calls.push_back("discard_recovered " + id); }
    void retry_recovery_scan() override { calls.push_back("retry_recovery_scan"); }

    fv::MeetingControlSignals events() override {
        return {meeting_state_changed,
                meeting_status_update,
                status_update,
                meeting_error,
                meeting_server_started,
                meeting_recovery_found,
                meeting_recovery_scan_failed,
                meeting_guest_link_ready,
                meeting_platform_ack_requested,
                past_meetings_refresh_requested,
                playback_stop_requested};
    }

    fv::Signal<fv::Json> meeting_state_changed;
    fv::Signal<std::string> meeting_status_update;
    fv::Signal<std::string> status_update;
    fv::Signal<std::string> meeting_error;
    fv::Signal<fv::Json> meeting_server_started;
    fv::Signal<std::vector<fv::Json>> meeting_recovery_found;
    fv::Signal<std::string> meeting_recovery_scan_failed;
    fv::Signal<std::string> meeting_guest_link_ready;
    fv::Signal<> meeting_platform_ack_requested;
    fv::Signal<> past_meetings_refresh_requested;
    fv::Signal<std::string> playback_stop_requested;
};

// History with no entries, so the shell builds its sidebar and Past Meetings.
class EmptyHistoryStore final : public fv::HistoryStore {
public:
    explicit EmptyHistoryStore(std::filesystem::path folder) : folder_(std::move(folder)) {}
    std::vector<fv::HistoryEntry> get_history(std::optional<std::int64_t>) override { return {}; }
    std::vector<fv::HistoryEntry> search(std::string_view, std::optional<std::int64_t>) override {
        return {};
    }
    std::optional<fv::HistoryEntry> get_entry(std::string_view) override { return std::nullopt; }
    std::optional<fv::HistoryEntry> last_dictation() override { return std::nullopt; }
    std::optional<fv::HistoryEntry> use_version(std::string_view, std::string_view) override {
        return std::nullopt;
    }
    bool delete_entry(std::string_view, bool) override { return false; }
    void clear_history() override {}
    void clear_history_and_recordings() override {}
    std::optional<std::filesystem::path> get_recording_path(std::string_view) override {
        return std::nullopt;
    }
    std::filesystem::path recordings_folder() const override { return folder_; }

private:
    std::filesystem::path folder_;
};

inline fv::meeting::PlatformInfo windowsPlatform() {
    return {"win32", "AMD64", ""};
}

// The shell, History and a presenter over RecordingControls. `saved` starts the settings
// (setup done, intro seen, platform acknowledged unless it says otherwise).
class PresenterRig {
public:
    explicit PresenterRig(fv::Json saved = fv::Json::object(),
                          fv::MeetingPresenter::Hooks hooks = {}, bool withHistory = true,
                          bool withGrants = true,
                          const std::function<void(fv::AppContext&)>& beforePresenter = {})
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())),
          settings_(withDefaults(std::move(saved))), ui_(qApp), io_("presenter-test", 1),
          audio_(std::make_shared<fv::audio::testing::FakeBackend>()),
          database_(offGui([this] { return fv::Database::open_or_create(root_.path() / "m.db"); })),
          context_(services()) {
        repository_ = std::make_shared<fv::meeting_state::SqlMeetingRepository>(*database_);
        auto backend = std::make_unique<fv::clipboard::FakeClipboardBackend>();
        clipboardBackend_ = backend.get();
        context_.provide(std::make_shared<fv::clipboard::TemporaryClipboard>(
            std::move(backend), fv::clipboard::SequenceSource{}, fv::clipboard::Renderer{}, nullptr,
            false));
        shell_.provide(context_);
        if (withHistory) {
            history_.provide(context_);
            context_.provide<fv::HistoryStore>(
                std::make_shared<EmptyHistoryStore>(root_.path() / "recordings"));
        }
        // Quick Record's page as the fit measures it: a scroll area over a short column.
        context_.screens().addScreen(fv::ScreenSpec{fv::order::tab::quick_record,
                                                    QStringLiteral("Quick Record"),
                                                    [](QWidget* parent) -> QWidget* {
                                                        auto* page = new QScrollArea(parent);
                                                        page->setWidgetResizable(true);
                                                        auto* column = new QWidget;
                                                        column->setMinimumHeight(160);
                                                        page->setWidget(column);
                                                        return page;
                                                    },
                                                    {},
                                                    {}});
        shell_.install(context_);
        if (withHistory) {
            history_.install(context_);
        }
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        if (!hooks.platform) {
            hooks.platform = windowsPlatform();
        }
        // Services the presenter finds as it installs (a paired host's records).
        if (beforePresenter) {
            beforePresenter(context_);
        }
        controls_ = std::make_shared<RecordingControls>();
        presenter_ = new fv::MeetingPresenter(
            context_, window(), controls_, {}, repository_, root_.path() / "meetings",
            withGrants ? std::make_shared<fv::MeetingConsentGrants>() : nullptr, std::move(hooks));
        presenter_->install();
        window().completeSetup();
    }
    ~PresenterRig() {
        window().close();
        QApplication::processEvents();
        io_.shutdown(std::chrono::milliseconds(2000));
        offGui([&] { database_->close(); });
    }
    PresenterRig(const PresenterRig&) = delete;
    PresenterRig& operator=(const PresenterRig&) = delete;

    static fv::Json withDefaults(fv::Json saved) {
        const fv::Json defaults{{"meeting_setup_complete", true},
                                {"meeting_mode_intro_seen", true},
                                {"meeting_unsupported_platform_ack", true},
                                {"meeting_speaker_id_backend", "off"}};
        for (const auto& [key, value] : defaults.items()) {
            if (!saved.contains(key)) {
                saved[key] = value;
            }
        }
        return saved;
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    fv::ActivityState& activity() { return activity_; }
    RecordingControls& controls() { return *controls_; }
    fv::MeetingPresenter& presenter() { return *presenter_; }
    fv::clipboard::FakeClipboardBackend& clipboard() { return *clipboardBackend_; }
    fv::ThreadPool& io() { return io_; }
    fv::meeting_state::SqlMeetingRepository& repository() { return *repository_; }

    // Builds and shows the Meeting Mode tab.
    fv::MeetingModeTab* openMeetingTab() {
        auto* tabs = window().tabbedContent();
        tabs->setCurrentIndex(fv::TabbedContentWidget::TabMeetingMode);
        QApplication::processEvents();
        return qobject_cast<fv::MeetingModeTab*>(
            window().page(fv::TabbedContentWidget::TabMeetingMode));
    }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.io = &io_;
        made.activity = &activity_;
        made.audio = audio_.get();
        made.clock = &fv::system_clock();
        made.db = &*database_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::ThreadPool io_;
    fv::ActivityState activity_;
    std::shared_ptr<fv::audio::testing::FakeBackend> audio_;
    std::optional<fv::Database> database_;
    std::shared_ptr<fv::meeting_state::SqlMeetingRepository> repository_;
    fv::clipboard::FakeClipboardBackend* clipboardBackend_ = nullptr;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::HistoryFeature history_;
    std::shared_ptr<RecordingControls> controls_;
    fv::MeetingPresenter* presenter_ = nullptr;
};

// Answers the next modal message box with `button` once it is up.
inline void answerNextBox(QObject* context, QMessageBox::StandardButton button) {
    QTimer::singleShot(0, context, [button] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            if (auto* chosen = box->button(button)) {
                chosen->click();
            }
        }
    });
}

} // namespace fvtest
