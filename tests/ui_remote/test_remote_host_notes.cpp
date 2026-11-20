// The sharing host's reachability notes on the Remote engine page (ui_qt/dialogs/settings_remote.py
// _show_host_notes with services/remote_asr/reachability.py's notes): a public Windows network
// says so with "Open network settings", which opens the adapter's Settings page; a VPN and
// discovery that couldn't listen say what to do, without a button.
#include <catch2/catch_test_macros.hpp>

#include <QPushButton>

#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/remote_engine_page.h"
#include "ui_remote_support.h"
#include "ui_widgets/wrapped_label.h"

using fvtest::PageHarness;
using fvtest::pumpUntil;

TEST_CASE("the sharing host's notes show each fix, and the public network's opens Settings",
          "[remote_page][widget]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.notes = {
            {"public_network",
             "Windows treats \"Home\" as a public network, which usually keeps other computers "
             "from connecting here. If they can't, make it a private network.",
             "Open network settings", "ms-settings:network-wifi"},
            {"vpn",
             "A VPN is on. If other computers can't connect, allow local network (LAN) access in "
             "its settings.",
             "", ""},
            {"discovery",
             "Other computers can't find this one by searching (UDP port 47821 is in use). They "
             "can still enter its address and pairing code.",
             "", ""}};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    QWidget* notes = harness.page->hostNotes();
    REQUIRE(pumpUntil([&] { return !notes->isHidden(); }));
    QStringList texts;
    for (auto* label : notes->findChildren<fv::WrappedLabel*>(QStringLiteral("remoteHostNote"))) {
        texts.append(label->text());
    }
    REQUIRE(texts.size() == 3);
    CHECK(texts[0].startsWith(QStringLiteral("Windows treats \"Home\" as a public network")));
    CHECK(texts[1].startsWith(QStringLiteral("A VPN is on.")));
    CHECK(texts[2].startsWith(QStringLiteral("Other computers can't find this one")));
    const auto buttons = notes->findChildren<QPushButton*>(QStringLiteral("remoteHostNoteButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons[0]->text() == QStringLiteral("Open network settings"));
    buttons[0]->click();
    CHECK(harness.opened == std::vector<QString>{QStringLiteral("ms-settings:network-wifi")});
}

TEST_CASE("a host that is not sharing shows no notes", "[remote_page][widget]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = false;
        engine.state.running = false;
        engine.state.notes = {{"vpn", "A VPN is on.", "", ""}};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    REQUIRE(harness.settle());
    CHECK(harness.page->hostNotes()->isHidden());
}
