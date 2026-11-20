// Saving the dictation engine is what tells every view of it: the settings watch, and the Quick
// Record and Upload File tabs following what is saved.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QSignalSpy>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "record_fixture.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "ui_record/engine_selection.h"
#include "ui_record/upload_file_tab.h"

namespace {

using fv::Json;

// Runs every task at once on the posting thread and is always current: Python's synchronous
// watcher callbacks.
class InlineExecutor final : public fv::Executor {
public:
    void post(fv::Task task) override { task(); }
    fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

constexpr const char* kLanguage = "local_asr_language";
constexpr const char* kSelected = "selected_model";

} // namespace

// from
// tests/test_engine_selection_sync.py::TestWatch::test_a_save_that_changes_a_watched_key_is_heard
TEST_CASE("a save that changes a watched key is heard", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch = store.watch({kLanguage, kSelected}, executor,
                             [&](const Json& changed) { heard.push_back(changed); });

    store.save_setting(kLanguage, "ru");
    store.update(Json{{kSelected, "remote"}, {"ui_theme", "dark"}});

    CHECK(heard == std::vector<Json>{Json::array({kLanguage}), Json::array({kSelected})});
}

// from
// tests/test_engine_selection_sync.py::TestWatch::test_the_new_value_is_saved_before_the_watcher_runs
TEST_CASE("the new value is saved before the watcher runs", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    std::vector<std::string> seen;
    auto watch = store.watch({kLanguage}, executor, [&](const Json&) {
        seen.push_back(store.get(fv::keys::local_asr_language));
    });
    store.save_setting(kLanguage, "ru");
    CHECK(seen == std::vector<std::string>{"ru"});
}

// from
// tests/test_engine_selection_sync.py::TestWatch::test_saving_the_same_value_or_another_key_is_not_heard
TEST_CASE("saving the same value or another key is not heard", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store(Json{{kLanguage, "ru"}});
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch =
        store.watch({kLanguage}, executor, [&](const Json& changed) { heard.push_back(changed); });

    store.save_setting(kLanguage, "ru");
    store.save_setting("ui_theme", "dark");

    CHECK(heard.empty());
}

// from
// tests/test_engine_selection_sync.py::TestWatch::test_a_change_inside_a_saved_mapping_is_heard
TEST_CASE("a change inside a saved mapping is heard", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store(Json{{"local_asr_models", {{"parakeet", "parakeet-v3"}}}});
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch = store.watch({"local_asr_models"}, executor,
                             [&](const Json& changed) { heard.push_back(changed); });

    store.mutate([](Json& settings) { settings["local_asr_models"]["parakeet"] = "orukeet-v0.1"; });
    CHECK(heard == std::vector<Json>{Json::array({"local_asr_models"})});
}

// from
// tests/test_engine_selection_sync.py::TestWatch::test_saving_none_over_an_unset_key_and_removing_a_key_are_changes
TEST_CASE("saving none over an unset key and removing a key are changes",
          "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch =
        store.watch({kLanguage}, executor, [&](const Json& changed) { heard.push_back(changed); });

    store.save_setting(kLanguage, nullptr);
    store.update(Json::object(), {kLanguage});

    CHECK(heard.size() == 2);
}

// from tests/test_engine_selection_sync.py::TestWatch::test_replacing_every_setting_is_heard
TEST_CASE("replacing every setting is heard", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store(Json{{kSelected, "parakeet"}});
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch =
        store.watch({kSelected}, executor, [&](const Json& changed) { heard.push_back(changed); });

    store.save_all(Json{{kSelected, "parakeet"}, {"ui_theme", "dark"}});
    store.save_all(Json{{kSelected, "api"}});

    CHECK(heard == std::vector<Json>{Json::array({kSelected})});
}

// from tests/test_engine_selection_sync.py::TestWatch::test_a_stopped_watch_hears_nothing
TEST_CASE("a stopped watch hears nothing", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch =
        store.watch({kLanguage}, executor, [&](const Json& changed) { heard.push_back(changed); });
    watch.disconnect();
    watch.disconnect();
    store.save_setting(kLanguage, "ru");
    CHECK(heard.empty());
}

// from
// tests/test_engine_selection_sync.py::TestWatch::test_a_failing_watcher_neither_blocks_the_save_nor_the_next_watcher
TEST_CASE("a failing watcher neither blocks the save nor the next watcher",
          "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    std::vector<Json> heard;
    auto failing =
        store.watch({kLanguage}, executor, [](const Json&) { throw std::runtime_error("gone"); });
    auto next =
        store.watch({kLanguage}, executor, [&](const Json& changed) { heard.push_back(changed); });

    store.save_setting(kLanguage, "ru");

    CHECK(store.get(fv::keys::local_asr_language) == "ru");
    CHECK(heard == std::vector<Json>{Json::array({kLanguage})});
}

// from tests/test_engine_selection_sync.py::TestWatch::test_a_failed_save_is_not_heard
TEST_CASE("a failed save is not heard", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    std::vector<Json> heard;
    auto watch =
        store.watch({kLanguage}, executor, [&](const Json& changed) { heard.push_back(changed); });

    CHECK_THROWS_AS(store.mutate([](Json& settings) {
        settings[kLanguage] = "ru";
        throw std::invalid_argument("no");
    }),
                    std::invalid_argument);
    CHECK(heard.empty());
}

// from tests/test_engine_selection_sync.py::TestWatch::test_a_watcher_may_save_without_deadlock
TEST_CASE("a watcher may save without deadlock", "[test_engine_selection_sync]") {
    fv::MemorySettingsStore store;
    InlineExecutor executor;
    auto watch = store.watch({kLanguage}, executor,
                             [&](const Json&) { store.save_setting("ui_theme", "dark"); });
    std::thread worker([&] { store.save_setting(kLanguage, "ru"); });
    worker.join();
    CHECK(store.snapshot()->at("ui_theme") == "dark");
}

namespace {

// The tabs the session has (Quick Record, then Upload File), following the saved engine through
// the relay.
struct Views {
    explicit Views(const Json& saved = Json::object())
        : settings(saved), ui(&context), selection(settings, ui) {
        tabs.push_back(std::make_unique<fv::QuickRecordTab>(settings, fv::speechEngineCatalog()));
        tabs.push_back(std::make_unique<fv::UploadFileTab>(settings, fv::speechEngineCatalog()));
        for (const auto& tab : tabs) {
            selection.add(tab.get());
            tab->syncEngineSelection();
        }
    }
    ~Views() { tabs.clear(); }

    fv::MemorySettingsStore settings;
    QObject context;
    fv::qt::QtUiExecutor ui;
    fv::EngineSelection selection;
    std::vector<std::unique_ptr<fv::TranscriptionTabBase>> tabs;
};

} // namespace

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_a_saved_backend_shows_on_every_tab
TEST_CASE("a saved backend shows on every tab", "[test_engine_selection_sync][widget]") {
    Views views(Json{{"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}});
    for (const auto& tab : views.tabs) {
        REQUIRE(tab->localEngine()->modelCombo()->currentData().toString() ==
                QStringLiteral("orukeet-v0.1"));
    }
    // The model comes back to its default while the tabs are not told: an outside edit.
    views.settings.set_contents(Json::object());
    fv::save_model_selection(views.settings, "parakeet");
    for (const auto& tab : views.tabs) {
        CHECK(tab->currentBackend() == QStringLiteral("Parakeet"));
        CHECK(tab->localEngine()->modelCombo()->currentData().toString() ==
              QStringLiteral("parakeet-v3"));
    }
}

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_a_backend_without_local_fields_hides_them_on_every_tab
TEST_CASE("a backend without local fields hides them on every tab",
          "[test_engine_selection_sync][widget]") {
    Views views;
    fv::save_model_selection(views.settings, "api");
    for (const auto& tab : views.tabs) {
        CHECK(tab->currentBackend() == QStringLiteral("API"));
        CHECK(tab->localEngine()->isHidden());
    }
}

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_a_model_and_device_chosen_on_one_tab_show_on_the_other
TEST_CASE("a model and device chosen on one tab show on the other",
          "[test_engine_selection_sync][widget]") {
    Views views;
    fv::TranscriptionTabBase& record = *views.tabs[0];
    fv::TranscriptionTabBase& other = *views.tabs[1];
    std::vector<std::string> reloads;
    QObject::connect(&record, &fv::TranscriptionTabBase::engineSettingsChanged, &record,
                     [&] { reloads.push_back("record"); });
    QObject::connect(&other, &fv::TranscriptionTabBase::engineSettingsChanged, &other,
                     [&] { reloads.push_back("other"); });

    QComboBox* model = record.localEngine()->modelCombo();
    model->setCurrentIndex(model->findData(QStringLiteral("orukeet-v0.1")));
    QComboBox* device = record.localEngine()->deviceCombo();
    device->setCurrentIndex(device->findData(QStringLiteral("cpu")));

    CHECK(other.localEngine()->modelCombo()->currentData().toString() ==
          QStringLiteral("orukeet-v0.1"));
    CHECK(other.localEngine()->deviceCombo()->currentData().toString() == QStringLiteral("cpu"));
    // Only the tab that was used asks for the engine reload.
    CHECK(reloads == std::vector<std::string>{"record", "record"});
}

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_a_device_the_controller_reverts_shows_on_every_tab
TEST_CASE("a device the controller reverts shows on every tab",
          "[test_engine_selection_sync][widget]") {
    Views views;
    views.settings.update(Json{{"local_asr_devices", {{"parakeet", "cpu"}}}});
    for (const auto& tab : views.tabs) {
        CHECK(tab->localEngine()->deviceCombo()->currentData().toString() == QStringLiteral("cpu"));
    }
}

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_a_save_from_a_worker_thread_reaches_the_tabs
TEST_CASE("a save from a worker thread reaches the tabs", "[test_engine_selection_sync][widget]") {
    Views views;
    std::thread worker(
        [&] { views.settings.update(Json{{"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}}); });
    worker.join();
    QApplication::processEvents();
    for (const auto& tab : views.tabs) {
        CHECK(tab->localEngine()->modelCombo()->currentData().toString() ==
              QStringLiteral("orukeet-v0.1"));
    }
}

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_a_saved_engine_does_not_announce_a_choice
TEST_CASE("a saved engine does not announce a choice", "[test_engine_selection_sync][widget]") {
    Views views;
    std::vector<QString> chosen;
    for (const auto& tab : views.tabs) {
        QObject::connect(tab.get(), &fv::TranscriptionTabBase::modelChanged, tab.get(),
                         [&](const QString& name) { chosen.push_back(name); });
    }
    fv::save_model_selection(views.settings, "api");
    fv::save_model_selection(views.settings, "parakeet");
    CHECK(chosen.empty());
}

// from
// tests/test_engine_selection_sync.py::TestTabsFollowTheSavedEngine::test_an_unrelated_save_leaves_the_fields_alone
TEST_CASE("an unrelated save leaves the fields alone", "[test_engine_selection_sync][widget]") {
    Views views;
    fv::TranscriptionTabBase& record = *views.tabs[0];
    QComboBox* model = record.localEngine()->modelCombo();
    model->blockSignals(true);
    model->setCurrentIndex(model->findData(QStringLiteral("orukeet-v0.1")));
    model->blockSignals(false);
    views.settings.save_setting("ui_theme", "dark");
    CHECK(model->currentData().toString() == QStringLiteral("orukeet-v0.1"));
}

// from
// tests/test_engine_selection_sync.py::test_the_main_window_tabs_follow_a_choice_once_it_is_saved
TEST_CASE("the tabs follow a choice once it is saved", "[test_engine_selection_sync][widget]") {
    Views views;
    fv::TranscriptionTabBase& record = *views.tabs[0];
    fv::TranscriptionTabBase& other = *views.tabs[1];
    // The controller saves a choice it accepts; stand in for it.
    QObject::connect(&record, &fv::TranscriptionTabBase::modelChanged, &record,
                     [&](const QString& name) {
                         fv::save_model_selection(views.settings, name.toLower().toStdString());
                     });
    record.modelCombo()->setCurrentIndex(record.modelCombo()->findData(QStringLiteral("api")));

    CHECK(other.currentBackend() == QStringLiteral("API"));
    CHECK(other.localEngine()->isHidden());
    CHECK(views.settings.snapshot()->at("selected_model") == "api");
}

TEST_CASE("any view can follow the saved engine until it is deleted",
          "[test_engine_selection_sync][widget]") {
    Views views;
    int reloads = 0;
    auto page = std::make_unique<QObject>();
    views.selection.follow(page.get(), [&reloads] { ++reloads; });
    // A page is no tab: views() lists the tabs alone.
    CHECK(views.selection.views().size() == 2);

    views.settings.update(Json{{"local_asr_devices", {{"parakeet", "cpu"}}}});
    CHECK(reloads == 1);
    // The dictation languages decide the Language field, so a save of them is heard too.
    views.settings.update(Json{{"dictation_languages", Json::array({"en", "es"})}});
    CHECK(reloads == 2);
    views.settings.save_setting("ui_theme", "dark");
    CHECK(reloads == 2);

    page.reset();
    views.settings.update(Json{{"local_asr_devices", {{"parakeet", "auto"}}}});
    CHECK(reloads == 2);
}

TEST_CASE("both recording tabs offer host model management only for Remote",
          "[test_engine_selection_sync][widget]") {
    Views views;
    // Built with the card, as OpenWhisper builds it, and hidden until Remote is chosen.
    for (const auto& tab : views.tabs) {
        auto* manage = tab->findChild<QPushButton*>(QStringLiteral("remoteModelNotice"));
        REQUIRE(manage != nullptr);
        CHECK(manage->isHidden());
    }

    views.settings.save_setting("selected_model", "remote");
    QCoreApplication::processEvents();
    for (const auto& tab : views.tabs) {
        auto* manage = tab->findChild<QPushButton*>(QStringLiteral("remoteModelNotice"));
        REQUIRE(manage != nullptr);
        CHECK_FALSE(manage->isHidden());
        QSignalSpy help(tab.get(), &fv::TranscriptionTabBase::helpRequested);
        manage->click();
        REQUIRE(help.size() == 1);
        CHECK(help.front().front().toString() == QStringLiteral("remote_models"));
        CHECK(views.settings.get(fv::keys::selected_model) == "remote");
    }

    views.settings.save_setting("selected_model", "api");
    QCoreApplication::processEvents();
    for (const auto& tab : views.tabs) {
        auto* manage = tab->findChild<QPushButton*>(QStringLiteral("remoteModelNotice"));
        REQUIRE(manage != nullptr);
        CHECK(manage->isHidden());
    }
}
