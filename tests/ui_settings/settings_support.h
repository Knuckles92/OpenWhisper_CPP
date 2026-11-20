#pragma once

// The Settings window over fakes: a data folder of its own, an in-memory settings file, the GUI
// executor and an AppContext whose SettingsPageRegistry holds stand-in pages. A stand-in is the
// page's own container with a group title and two tiles, as tools/parity/builders/a11.py builds
// it on OpenWhisper's side; the real pages register in later packages.

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QRect>
#include <QString>
#include <QWidget>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

namespace fvtest {

namespace dest = fv::destinations;

// OpenWhisper's page headings, as settings_dialog.py registers them (page modules' TITLE and
// SUBTITLE for theirs), in _add_page order; the product name is FioriVoce's.
struct Heading {
    std::string_view key;
    const char* title;
    const char* subtitle;
    bool scroll = true;
};

inline const std::vector<Heading>& headings() {
    static const std::vector<Heading> all{
        {dest::overview, "Home",
         "Your everyday settings, and where your words go. Everything else is in the sidebar."},
        {dest::voice_model, "Voice model",
         "The transcription engine used by Quick Record, hotkey dictation, and Upload File."},
        {dest::remote_engine, "Remote engine",
         "Transcribe dictation and meetings with another computer's engine over your network or "
         "Tailscale, or share this computer's engine with computers you pair."},
        {dest::walkthroughs, "Walkthroughs",
         "Talk while you take screenshots, then paste one document into your agent: its "
         "shortcuts, how the document looks, and the optional AI write-up."},
        {dest::recording, "Recording", "Microphone, saved audio retention, and the live preview."},
        {dest::cleanup, "AI cleanup",
         "Rewrite a finished dictation with a language model, together with your learned rules."},
        {dest::cleanup_rules, "Learned rules",
         "Teach FioriVoce your preferred spellings, terminology, and formatting. Applied "
         "whenever AI cleanup runs."},
        {dest::cleanup_profiles, "Profiles",
         "Turn a recording into a support ticket, email, or your own format."},
        {dest::dictionary, "Dictionary",
         "Names, jargon, and spellings FioriVoce should get right every time."},
        {dest::snippets, "Snippets",
         "Say a short phrase to insert text you use often, exactly as written."},
        {dest::styles, "Apps & styles",
         "Know which app you're dictating into, and write in a tone that fits it."},
        {dest::commands, "Commands",
         "Rewrite selected text by voice, and keep the rewrites you use often."},
        {dest::actions, "Actions",
         "Send what you say somewhere other than the cursor: a chat, your agent, or your own "
         "tools."},
        {dest::hooks, "Hooks",
         "Tell your own tools when you dictate, and let them change the words before they're "
         "pasted."},
        {dest::meeting_voice, "Voice & speakers",
         "The speech engine for live captions and the optional end-of-meeting re-transcription: "
         "dictation's, or one of its own. Applies to your next meeting."},
        {dest::meeting_intelligence, "Intelligence",
         "Pi or a coding agent you already use runs every Meeting Mode pass. Nothing is sent "
         "until you turn on AI insights for a meeting."},
        {dest::meeting_fast, "Fast judgments",
         "Optional checks by Jev or your own text model. Each feature describes the text it "
         "shares."},
        {dest::meeting_after, "After the meeting",
         "Steps that run after End, once live captions have finished."},
        {dest::meeting_dashboard, "Dashboard",
         "Who can open the live meeting dashboard, and on which port."},
        {dest::downloads, "Downloads",
         "Speech models and optional components, downloaded on demand.", false},
        {dest::runtime, "Runtime",
         "How local Whisper models run. Dictation and Meeting Mode both inherit these."},
        {dest::backup, "Backup & restore",
         "Save a local copy of your data or inspect a backup before restoring it."},
        {dest::general, "General",
         "How finished transcriptions leave the app and how the window behaves when you close "
         "it."},
        {dest::appearance, "Appearance",
         "Theme, text size, and how the overlay near your pointer looks while you dictate."},
        {dest::hotkeys, "Hotkeys",
         "Every global shortcut in one place, including the ones you give your profiles, "
         "transforms and actions."},
        {dest::api_keys, "API keys",
         "Credentials for cloud providers and custom endpoints. Saved keys live in your "
         "operating system's credential manager, never in the settings file."},
        {dest::mcp, "API & MCP",
         "Let agents and your own programs read history, follow live events and run actions."},
        {dest::advanced, "Developer options", "Meeting re-transcription and developer tools."},
    };
    return all;
}

inline const char* kStandInGroup = "Stand-in group";

// The stand-in's content, added to the page itself (_tile_group on the page's layout); returns
// the page. extraTiles adds tiles after the builder's two, for a page that must scroll.
inline QWidget* fillStandIn(QWidget* parent, int extraTiles = 0) {
    auto* page = qobject_cast<fv::SettingsPage*>(parent);
    std::vector<QWidget*> tiles{
        new fv::InfoTile(QStringLiteral("First tile"),
                         QStringLiteral("A tile of the page that registers here.")),
        new fv::InfoTile(QStringLiteral("Second tile"),
                         QStringLiteral("Another tile of the same page.")),
    };
    for (int index = 0; index < extraTiles; ++index) {
        tiles.push_back(new fv::InfoTile(QStringLiteral("More %1").arg(index + 1),
                                         QStringLiteral("A tile that makes the page taller.")));
    }
    page->addTileGroup(page->pageLayout(), QString::fromUtf8(kStandInGroup), std::move(tiles));
    return page;
}

inline fv::SettingsPageSpec standInSpec(const Heading& heading) {
    fv::SettingsPageSpec spec;
    spec.destination = heading.key;
    spec.title = QString::fromUtf8(heading.title);
    spec.subtitle = QString::fromUtf8(heading.subtitle);
    spec.scroll = heading.scroll;
    spec.create = [](QWidget* parent) {
        return fillStandIn(parent);
    };
    return spec;
}

// A page module that records each hook it is asked for, as the fake page module of
// test_personalize_foundation_ui_settings.py does; its tile is the control "fake_page_tile".
class RecordingPage : public QWidget, public fv::SettingsPageModule {
public:
    explicit RecordingPage(std::vector<std::string>& calls, QWidget* parent = nullptr)
        : QWidget(parent), calls_(calls) {
        calls_.push_back("build");
        tile = new fv::InfoTile(QStringLiteral("Fake words tile"), QStringLiteral("zebra quokka"),
                                {}, this);
    }

    void loadSettings(const fv::Json& settings) override {
        lastLoaded = settings;
        record("load");
    }
    void refreshPage() override { record("refresh"); }
    void cancelCapture() override { record("cancel"); }
    QWidget* namedControl(std::string_view name) override {
        return name == "fake_page_tile" ? tile : nullptr;
    }

    // Every hook throws ValueError("broken page") after recording "broken".
    bool broken = false;
    fv::InfoTile* tile = nullptr;
    fv::Json lastLoaded;

private:
    void record(const char* call) {
        if (broken) {
            calls_.push_back("broken");
            throw std::runtime_error("broken page");
        }
        calls_.push_back(call);
    }

    std::vector<std::string>& calls_;
};

// What Bootstrap builds for Settings, without the rest: a data folder of its own, an in-memory
// settings file, the GUI executor, an AppContext.
class SettingsRig {
public:
    explicit SettingsRig(const fv::Json& settings = fv::Json::object())
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), settings_(settings),
          ui_(qApp), context_(services()) {}

    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    fv::SettingsPageRegistry& registry() { return context_.settingsPages(); }

    // Registers a stand-in for every destination OpenWhisper's rail has, each first passed to
    // customize (to give it a rail value, controls or another create).
    void registerStandIns(const std::function<void(fv::SettingsPageSpec&)>& customize = {}) {
        for (const Heading& heading : headings()) {
            fv::SettingsPageSpec spec = standInSpec(heading);
            if (customize) {
                customize(spec);
            }
            registry().addPage(std::move(spec));
        }
    }

    // The dialog on a 1920 x 1080 screen, so its first-show fit leaves it as built.
    std::unique_ptr<fv::SettingsDialog> dialog() {
        auto made = std::make_unique<fv::SettingsDialog>(context_);
        made->availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        return made;
    }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
};

// The application styled the way OpenWhisper's QtApplication styles it, for one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

inline void processEvents(int times = 1) {
    for (int index = 0; index < times; ++index) {
        QApplication::processEvents();
    }
}

// QTest.qWait: the event loop turned for ms.
inline void wait(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

inline QString qs(std::string_view text) {
    return fv::qt::toQString(text);
}

} // namespace fvtest
