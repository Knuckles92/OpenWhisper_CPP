#pragma once

// The Settings window with the App pages and the Walkthroughs page over fakes: the installed
// agents are a fixed scan (agents::FakeInstalledAgents, which starts nothing), the page's scan runs
// on the rig's worker pool, and every other destination is a stand-in page. The real Hotkeys page
// is among the App pages, so the Walkthroughs page's shortcut rows are the ones it hands out.

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QComboBox>
#include <QString>

#include "agents/testing/fake_installed_agents.h"
#include "app_pages_support.h"
#include "ui_settings/walkthroughs_page.h"

namespace fvtest {

// An agent the scan found.
inline fv::agents::InstalledAgent foundAgent(const std::string& id) {
    return fv::agents::InstalledAgent{id, "C:/agents/" + id + ".exe", "1.0.0", "", true, ""};
}

// What a scan finds when every agent is installed, and when only Codex is missing.
inline fv::agents::ScanResult everyAgent() {
    return {{"claude_code", foundAgent("claude_code")},
            {"codex", foundAgent("codex")},
            {"opencode_cli", foundAgent("opencode_cli")}};
}
inline fv::agents::ScanResult withoutCodex() {
    return {{"claude_code", foundAgent("claude_code")},
            {"codex", std::nullopt},
            {"opencode_cli", foundAgent("opencode_cli")}};
}

class WalkthroughsRig {
public:
    explicit WalkthroughsRig(const fv::Json& settings = fv::Json::object(),
                             fv::agents::ScanResult scan = everyAgent(), bool withAgents = true)
        : agents_(std::make_shared<fv::agents::FakeInstalledAgents>(std::move(scan))),
          withAgents_(withAgents), rig_(settings, [this](AppPagesRig& rig) { install(rig); }) {}

    AppPagesRig& rig() { return rig_; }
    fv::SettingsDialog& dialog() { return rig_.dialog(); }
    fv::agents::FakeInstalledAgents& agents() { return *agents_; }

    // The Walkthroughs page, built, with the scan it started finished.
    fv::WalkthroughsPage& page() {
        auto& built = rig_.page<fv::WalkthroughsPage>(dest::walkthroughs);
        settle();
        return built;
    }
    // Waits for the scan the page started, if it started one, to end.
    void settle() {
        REQUIRE(pumpUntil([this] { return !withAgents_ || agents_->cached_scan().has_value(); }));
        // The scan is cached on the worker before it posts the relabel to the GUI thread, so a
        // loaded machine can reach here first: wait until the names say what the scan found.
        REQUIRE(pumpUntil([this] { return labelled(); }));
        flush();
    }
    fv::Json saved(std::string_view key) { return rig_.saved(key); }
    bool has(std::string_view key) { return rig_.has(key); }

private:
    // Whether every name in the page's Agent combo is marked as the cached scan says (always,
    // without agents or a built page).
    bool labelled() {
        const std::optional<fv::agents::ScanResult> found = agents_->cached_scan();
        auto* page =
            dynamic_cast<fv::WalkthroughsPage*>(rig_.dialog().pageContent(dest::walkthroughs));
        if (!withAgents_ || !found || page == nullptr) {
            return true;
        }
        const QComboBox& combo = *page->agentCombo();
        for (int index = 0; index < combo.count(); ++index) {
            const auto it = found->find(combo.itemData(index).toString().toStdString());
            const bool present = it != found->end() && it->second.has_value();
            const QString marked = QString::fromUtf8(
                fv::WalkthroughsPage::kNotInstalled.data(),
                static_cast<qsizetype>(fv::WalkthroughsPage::kNotInstalled.size()));
            if (combo.itemText(index).endsWith(marked) == present) {
                return false;
            }
        }
        return true;
    }

    void install(AppPagesRig& rig) {
        fv::WalkthroughsOptions options;
        if (withAgents_) {
            options.agents = agents_;
        }
        options.worker = &rig.pool();
        fv::registerWalkthroughsPage(rig.context(), std::move(options));
    }

    std::shared_ptr<fv::agents::FakeInstalledAgents> agents_;
    bool withAgents_;
    AppPagesRig rig_;
};

} // namespace fvtest
