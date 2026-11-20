// The values the contract headers fix: defaults, orders and tables that later packages build
// on, checked against OpenWhisper's source. Each header also compiles alone (CMakeLists.txt).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "appkit/destinations.h"
#include "appkit/order.h"
#include "audio/backend.h"
#include "audio/pcm_tape.h"
#include "audio/resampler.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/events.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/paths.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "db/database.h"
#include "engine/platform.h"
#include "http/client.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "net/tls.h"
#include "net/ws_client.h"
#include "process/launcher.h"
#include "settings/keys.h"
#include "settings/setting.h"
#include "settings/store.h"
#include "speech/service.h"
#include "sql/db.h"

namespace dest = fv::destinations;

namespace {

template <class T>
constexpr bool is_move_only = std::is_move_constructible_v<T> && !std::is_copy_constructible_v<T> &&
                              !std::is_copy_assignable_v<T>;

} // namespace

// The seams every package fakes are interfaces, and the RAII handles cannot be copied.
static_assert(std::is_abstract_v<fv::EnvReader>);
static_assert(std::is_abstract_v<fv::Clock>);
static_assert(std::is_abstract_v<fv::Executor>);
static_assert(std::is_abstract_v<fv::EventSink>);
static_assert(std::is_abstract_v<fv::log::Sink>);
static_assert(std::is_abstract_v<fv::CredentialBackend>);
static_assert(std::is_abstract_v<fv::http::Client>);
static_assert(std::is_abstract_v<fv::http::BodySink>);
static_assert(std::is_abstract_v<fv::net::Responder>);
static_assert(std::is_abstract_v<fv::net::EventStream>);
static_assert(std::is_abstract_v<fv::net::WsConnection>);
static_assert(std::is_abstract_v<fv::net::WsClientConnection>);
static_assert(std::is_abstract_v<fv::Process>);
static_assert(std::is_abstract_v<fv::ProcessLauncher>);
static_assert(std::is_abstract_v<fv::Shell>);
static_assert(std::is_abstract_v<fv::audio::Backend>);
static_assert(std::is_abstract_v<fv::audio::Stream>);
static_assert(std::is_abstract_v<fv::audio::PlaybackSource>);
static_assert(std::is_abstract_v<fv::speech::Backend>);
static_assert(std::is_base_of_v<fv::Executor, fv::ThreadPool>);
static_assert(std::is_base_of_v<fv::Executor, fv::Strand>);
static_assert(std::is_base_of_v<fv::net::WsConnection, fv::net::WsClientConnection>);
static_assert(is_move_only<fv::Subscription>);
static_assert(is_move_only<fv::TimerHandle>);
static_assert(is_move_only<fv::sql::Connection>);
static_assert(is_move_only<fv::sql::Statement>);
static_assert(is_move_only<fv::sql::Transaction>);
static_assert(is_move_only<fv::speech::EngineLease>);
static_assert(std::is_same_v<fv::Json, nlohmann::ordered_json>);
static_assert(std::is_same_v<fv::audio::CaptureFn, void (*)(void*, const std::int16_t*, std::size_t,
                                                            fv::audio::BlockStatus)>);

TEST_CASE("speech lanes are ordered by priority, Final first and Warmup last") {
    using fv::speech::Lane;
    const std::vector<Lane> policy{Lane::Final,       Lane::RemoteClient, Lane::Meeting,
                                   Lane::Incremental, Lane::Preview,      Lane::Background,
                                   Lane::Warmup};
    for (std::size_t i = 1; i < policy.size(); ++i) {
        CHECK(static_cast<int>(policy[i - 1]) < static_cast<int>(policy[i]));
    }
}

TEST_CASE("request and stream defaults match the architecture") {
    const fv::http::Request request;
    CHECK(request.method == "GET");
    CHECK(request.connect_timeout == std::chrono::milliseconds(10'000));
    CHECK(request.idle_timeout == std::chrono::milliseconds(120'000));
    CHECK_FALSE(request.follow_redirects);
    CHECK_FALSE(request.bypass_proxy);
    CHECK(request.max_body_bytes == 64u * 1024u * 1024u);
    CHECK(std::holds_alternative<std::monostate>(request.body));

    // OpenWhisper's capture: 44100 Hz, mono, 1024-frame blocks, WASAPI auto-convert on.
    const fv::audio::StreamConfig stream{.device_index = 0};
    CHECK(stream.rate == 44100);
    CHECK(stream.channels == 1);
    CHECK(stream.frames_per_block == 1024);
    CHECK(stream.wasapi_auto_convert);

    const fv::speech::AudioClip clip;
    CHECK(clip.rate == 16000);
    const fv::speech::Capabilities capabilities;
    CHECK(capabilities.max_window_s == 30.0);
    CHECK(capabilities.live_preview);
    CHECK_FALSE(capabilities.accepts_files);
    const fv::speech::DecodeJob job;
    CHECK(job.lane == fv::speech::Lane::Final);
    CHECK(job.coalesce_key.empty());

    const fv::ProcessSpec spec;
    CHECK(spec.own_tree);
    CHECK_FALSE(spec.pipes);
    CHECK_FALSE(spec.disown_on_clean_exit);
}

TEST_CASE("the main window's tabs keep OpenWhisper's order, Meeting Mode last") {
    using namespace fv::order;
    CHECK(tab::quick_record == 100);
    CHECK(tab::upload_file == 200);
    CHECK(tab::walkthrough == 300);
    CHECK(tab::meeting_mode == 400);
    CHECK(side_panel::history < side_panel::past_meetings);
    CHECK(side_panel::past_meetings < side_panel::walkthroughs);
}

TEST_CASE("menu and tray items keep OpenWhisper's order") {
    using namespace fv::order;
    const std::vector<int> file{
        file_menu::settings, file_menu::guided_setup,          file_menu::downloads,
        file_menu::hotkeys,  file_menu::separator_before_tray, file_menu::minimize_to_tray,
        file_menu::exit};
    const std::vector<int> view{view_menu::side_panel,
                                view_menu::history_calendar,
                                view_menu::compact_mode,
                                view_menu::host_mode,
                                view_menu::separator_before_windows,
                                view_menu::scratchpad,
                                view_menu::stats,
                                view_menu::separator_before_dashboard,
                                view_menu::open_meeting_dashboard};
    const std::vector<int> help{help_menu::check_for_updates, help_menu::about};
    const std::vector<int> tray_items{tray::show,
                                      tray::hide,
                                      tray::separator_before_recording,
                                      tray::toggle_recording,
                                      tray::copy_original,
                                      tray::walkthrough,
                                      tray::take_screenshot,
                                      tray::language,
                                      tray::separator_before_meeting,
                                      tray::meeting,
                                      tray::open_meeting_dashboard,
                                      tray::separator_before_windows,
                                      tray::scratchpad,
                                      tray::stats,
                                      tray::settings,
                                      tray::separator_before_exit,
                                      tray::update,
                                      tray::exit};
    for (const auto* menu : {&file, &view, &help, &tray_items}) {
        for (std::size_t i = 1; i < menu->size(); ++i) {
            CHECK((*menu)[i - 1] < (*menu)[i]);
        }
    }
    CHECK(tray_items.size() == 18);
}

TEST_CASE("the settings rail has Home and six chapters of 27 pages in OpenWhisper's order") {
    CHECK(dest::home.key == "overview");
    CHECK(dest::home.name == "Home");
    CHECK(dest::home.icon == "home-blue.svg");

    std::vector<std::string_view> titles;
    std::vector<std::string_view> keys;
    for (const auto& chapter : dest::rail) {
        titles.push_back(chapter.title);
        for (const auto& page : chapter.pages) {
            keys.push_back(page.key);
        }
    }
    CHECK(titles == std::vector<std::string_view>{"Dictation", "Personalize", "Automate",
                                                  "Meeting Mode", "Models & storage", "App"});
    CHECK(keys == std::vector<std::string_view>{"recording",
                                                "voice_model",
                                                "cleanup",
                                                "remote_engine",
                                                "walkthroughs",
                                                "dictionary",
                                                "snippets",
                                                "styles",
                                                "cleanup_rules",
                                                "cleanup_profiles",
                                                "commands",
                                                "actions",
                                                "hooks",
                                                "mcp",
                                                "meeting_voice",
                                                "meeting_intelligence",
                                                "meeting_fast",
                                                "meeting_after",
                                                "meeting_dashboard",
                                                "downloads",
                                                "runtime",
                                                "backup",
                                                "general",
                                                "appearance",
                                                "hotkeys",
                                                "api_keys",
                                                "advanced"});
    CHECK(std::set<std::string_view>(keys.begin(), keys.end()).size() == keys.size());
    CHECK(dest::rail[3].icon == "users-amber.svg");
    CHECK(dest::rail[5].pages[4].name == "Developer options");
    CHECK(dest::rail[1].pages[2].name == "Apps & styles");
}

TEST_CASE("under CTest each test case has a data folder of its own") {
    // fv_test's contract (architecture 9.1). Started by hand, the executable has no data folder.
    const auto data_dir = fv::env_utf8("FIORIVOCE_DATA_DIR");
    if (!data_dir) {
        SKIP("Run through ctest, which sets FIORIVOCE_DATA_DIR per test case.");
    }
    const std::filesystem::path folder = fv::path_from_utf8(*data_dir);
    CHECK(std::filesystem::is_directory(folder));
    // <build>/tests/contracts/data/<16 hex digits of the SHA-1 of the test's name>
    CHECK(folder.parent_path().filename() == "data");
    CHECK(folder.filename().native().size() == 16);
}

TEST_CASE("a test executable starts with per-user folders of its own") {
    // cmake/fv_test_isolation.cpp, compiled into every test executable (lead amendment 25): the
    // process environment leads to a private folder, never to the user's real ones.
    const auto home = fv::env_utf8("FV_TEST_HOME");
    REQUIRE(home.has_value());
    const std::filesystem::path private_home =
        std::filesystem::weakly_canonical(fv::path_from_utf8(*home));
    const auto under_home = [&](const std::filesystem::path& path) {
        const auto relative =
            std::filesystem::weakly_canonical(path).lexically_relative(private_home);
        return !relative.empty() && *relative.begin() != "..";
    };
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    CHECK(under_home(paths.local_app_dir()));
    CHECK(under_home(paths.components_root()));
    REQUIRE_FALSE(paths.openwhisper_roots().empty());
    for (const auto& root : paths.openwhisper_roots()) {
        CHECK(under_home(root));
    }
    for (const char* removed : {"OPENWHISPER_DATA_DIR", "OPENAI_API_KEY", "GROQ_API_KEY",
                                "FIORIVOCE_RUNTIME_DIR", "CLAUDE_CONFIG_DIR"}) {
        CAPTURE(removed);
        CHECK_FALSE(fv::env_utf8(removed).has_value());
    }
}

TEST_CASE("legacy destination names resolve as settings_destinations.resolve_destination") {
    STATIC_CHECK(dest::resolve_destination("ondemand") == dest::voice_model);
    CHECK(dest::resolve_destination("text") == "cleanup");
    CHECK(dest::resolve_destination("meeting") == "meeting_voice");
    CHECK(dest::resolve_destination("library") == "downloads");
    CHECK(dest::resolve_destination("voice") == "downloads");
    CHECK(dest::resolve_destination("engine_downloads") == "downloads");
    CHECK(dest::resolve_destination("hotkeys") == "hotkeys");
    CHECK(dest::resolve_destination("no_such_page") == "no_such_page");
}
