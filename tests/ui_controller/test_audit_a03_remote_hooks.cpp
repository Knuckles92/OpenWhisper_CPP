// A03-47 (ui_controller.py set_remote_link, set_remote_clients and _on_remote_retry): what the
// Remote engine reports reaches the record tabs' engine card through the UI controller, and the
// card's retry reaches the engine's link.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "controller_support.h"
#include "ui_record/quick_record_tab.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

TEST_CASE("the remote link and the computers served reach the record tab's engine card",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    std::atomic<int> retries = 0;
    fv::remote::RemoteLink online;
    online.state = "connected";
    online.host = "devbox";
    online.route = "Tailscale";
    rig.app().set_remote_engine(fv::RemoteEngineHooks{
        .retry_now = [&retries] { ++retries; },
        .link = [online]() -> std::optional<fv::remote::RemoteLink> { return online; },
        .connected_clients =
            [] {
                return std::vector<fv::remote::ConnectedClient>{
                    {.device_id = "d1", .name = "Laptop", .busy = true}};
            },
    });
    REQUIRE(rig.startEngine());
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    CHECK(tab->remoteLinkText().isEmpty());

    // The service pokes from its own threads; the card is updated on the UI thread.
    std::thread([&rig] {
        rig.app().remote_link_poked();
        rig.app().remote_clients_poked();
    }).join();
    CHECK(waitUntil([tab] {
        return tab->remoteLinkText() == QStringLiteral("Connected to devbox · Tailscale");
    }));
    CHECK(waitUntil([tab] {
        return tab->servingLabel()->text() == QStringLiteral("Transcribing for Laptop");
    }));

    REQUIRE(rig.ui().onRemoteRetry);
    rig.ui().onRemoteRetry();
    CHECK(retries == 1);
}
