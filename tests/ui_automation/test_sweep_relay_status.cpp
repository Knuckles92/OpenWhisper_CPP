// Sweep B27: two widgets asking one ActionRelay for signing secrets each get their answer (the
// Settings editor and the Walkthrough tab share the session's relay), and the Actions page shows
// the Route by voice status that needs no credential store at once, as Python computes it on
// the spot, while the full answer is still on its way.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>

#include <QApplication>
#include <QObject>
#include <QSignalSpy>

#include "actions_page_support.h"
#include "base/testing/manual_executor.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_relay.h"
#include "ui_widgets/wrapped_label.h"

using fv::ActionRelay;
using fv::ActionRelayWork;
using fv::Json;
using namespace fvtest;

namespace {

ActionRelayWork secretsWork() {
    ActionRelayWork work;
    work.ensureSecret = [](const std::string& id) {
        return "whsec_" + id;
    };
    return work;
}

} // namespace

TEST_CASE("two requesters on one relay both get their secret", "[sweep][widget]") {
    fv::ManualExecutor io;
    ActionRelay relay(io, secretsWork());
    QObject settingsEditor;
    QObject walkthroughEditor;
    QSignalSpy done(&relay, &ActionRelay::secretDone);
    const int first = relay.nextToken();
    relay.fetchSecret(first, QStringLiteral("abc"), false, &settingsEditor);
    const int second = relay.nextToken();
    relay.fetchSecret(second, QStringLiteral("def"), false, &walkthroughEditor);
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 2);
    CHECK(done.at(0).at(0).toInt() == first);
    CHECK(done.at(0).at(1).toString() == QStringLiteral("whsec_abc"));
    CHECK(done.at(1).at(0).toInt() == second);
    CHECK(done.at(1).at(1).toString() == QStringLiteral("whsec_def"));

    // One requester's newer fetch still drops its own earlier answer, not the other's.
    relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"), false, &settingsEditor);
    const int other = relay.nextToken();
    relay.fetchSecret(other, QStringLiteral("def"), false, &walkthroughEditor);
    const int newest = relay.nextToken();
    relay.fetchSecret(newest, QStringLiteral("abc"), true, &settingsEditor);
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 4);
    CHECK(done.at(2).at(0).toInt() == other);
    CHECK(done.at(3).at(0).toInt() == newest);
}

TEST_CASE("a requester that is gone leaves the others' answers alone", "[sweep][widget]") {
    fv::ManualExecutor io;
    ActionRelay relay(io, secretsWork());
    QObject kept;
    QSignalSpy done(&relay, &ActionRelay::secretDone);
    {
        QObject closed;
        relay.fetchSecret(relay.nextToken(), QStringLiteral("abc"), false, &closed);
    }
    const int token = relay.nextToken();
    relay.fetchSecret(token, QStringLiteral("def"), false, &kept);
    io.run_all();
    QApplication::processEvents();
    REQUIRE(done.count() == 2);
    CHECK(done.at(1).at(0).toInt() == token);
}

TEST_CASE("the route by voice status that needs no key shows before the full answer",
          "[sweep][widget]") {
    ActionsRig rig(Json{{"voice_routing_enabled", true}, {"typesafe_enabled", false}});
    // The full answer waits until the test lets it go.
    auto release = std::make_shared<std::promise<void>>();
    std::shared_future<void> released = release->get_future().share();
    rig.setStatusFunction([released](const Json&) {
        released.wait_for(std::chrono::seconds(10));
        return fv::automation::RoutingStatus{true, true, "The full answer.", ""};
    });
    rig.dialog();
    fv::ActionsPage& page = rig.page();
    CHECK(page.routingStatusLine()->message()->text() ==
          QString::fromUtf8("Route by voice needs Fast judgments. Turn them on in Settings → "
                            "Meeting Mode → Fast judgments."));
    CHECK(page.routingStatusLine()->state() == QStringLiteral("attention"));
    release->set_value();
    CHECK(waitFor([&] {
        return page.routingStatusLine()->message()->text() == QStringLiteral("The full answer.");
    }));
}
