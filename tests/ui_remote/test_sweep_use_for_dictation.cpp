// The Remote engine page's "Use for dictation" through RemoteFeature's wiring
// (remote_feature.cpp): it saves the remote engine as the dictation model and reloads the engine,
// as OpenWhisper's page does (open item "Remote engine page: a pairing request waiting", its
// test-only part).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QApplication>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "models/testing/fake_engine_control.h"
#include "remote/control.h"
#include "remote/testing/fake_remote_engine.h"
#include "settings/model_selection.h"
#include "settings/testing/memory_store.h"
#include "ui_remote/remote_engine_page.h"
#include "ui_remote/remote_feature.h"
#include "ui_remote_support.h"
#include "ui_settings/settings_dialog.h"

namespace dest = fv::destinations;

TEST_CASE("Use for dictation saves the remote engine and reloads it", "[remote_page][widget]") {
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    fv::MemorySettingsStore settings;
    fv::qt::QtUiExecutor ui(qApp);
    fv::ManualExecutor io;
    fv::FakeClock clock;
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    services.settings = &settings;
    services.ui = &ui;
    services.io = &io;
    services.clock = &clock;
    fv::AppContext context(services);
    auto remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    remote->pairing = fvtest::pairedWith();
    context.provide<fv::remote::RemoteEngineControl>(remote);
    auto engine = std::make_shared<fv::models::FakeEngineControl>();
    context.provide<fv::models::EngineControl>(engine);
    fv::registerRemoteEnginePage(context);
    REQUIRE(fv::load_model_selection(settings) != "remote");

    fv::SettingsDialog dialog(context);
    dialog.ensurePage(dest::remote_engine);
    auto* page = dynamic_cast<fv::RemoteEnginePage*>(dialog.pageContent(dest::remote_engine));
    REQUIRE(page != nullptr);
    Q_EMIT page->useForDictationRequested();

    CHECK(fv::load_model_selection(settings) == "remote");
    CHECK(engine->reloads() == 1);
}
