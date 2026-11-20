// History and Past Meetings follow the paired computers' records (remote.py's records_changed
// wiring, audit A04-74): RemoteFeature::install connects the remote engine's and the record
// sync's "records", "client" and "host_renamed" changes to HistoryPanel::refresh and
// refreshPastMeetings; other changes leave them alone.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QCoreApplication>

#include "appkit/app_context.h"
#include "base/testing/manual_executor.h"
#include "remote/testing/fake_record_sync.h"
#include "remote/testing/fake_remote_engine.h"
#include "settings/testing/memory_store.h"
#include "ui_history/history_feature.h"
#include "ui_remote/remote_feature.h"

namespace {

struct RecordContext {
    fv::MemorySettingsStore settings{fv::Json::object()};
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    std::shared_ptr<fv::remote::testing::FakeRemoteEngine> engine =
        std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    std::shared_ptr<fv::remote::testing::FakeRecordSync> sync =
        std::make_shared<fv::remote::testing::FakeRecordSync>();
    std::shared_ptr<fv::HistoryPanel> panel = std::make_shared<fv::HistoryPanel>();
    fv::AppContext context{services()};
    int refreshes = 0;

    RecordContext() {
        // The calendar's refresh runs on both refresh() and refreshPastMeetings().
        panel->attachCalendar([] {}, [this] { ++refreshes; });
        context.provide<fv::remote::RemoteEngineControl>(engine);
        context.provide<fv::remote::RecordSyncControl>(sync);
        context.provide<fv::HistoryPanel>(panel);
        fv::RemoteFeature feature;
        feature.install(context);
    }
    fv::AppServices services() {
        fv::AppServices made;
        made.settings = &settings;
        made.io = &io;
        made.ui = &ui;
        return made;
    }
    // Runs what the change queued on the UI executor; the refreshes it caused.
    int after(const std::function<void()>& change) {
        ui.run_all();
        const int before = refreshes;
        change();
        ui.run_all();
        QCoreApplication::processEvents();
        return refreshes - before;
    }
};

} // namespace

TEST_CASE("remote record changes refresh History and Past Meetings", "[widget]") {
    RecordContext rig;
    for (const char* event : {"records", "client", "host_renamed"}) {
        INFO(event);
        CHECK(rig.after([&] { rig.engine->emit_changed(event); }) == 2);
    }
    CHECK(rig.after([&] { rig.sync->changed.emit("records"); }) == 2);
    // Anything else (the host's state, its clients' activity) is not History's.
    for (const char* event : {"state", "clients", "activity"}) {
        INFO(event);
        CHECK(rig.after([&] { rig.engine->emit_changed(event); }) == 0);
    }
}
