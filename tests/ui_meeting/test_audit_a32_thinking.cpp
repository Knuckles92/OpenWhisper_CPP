// Checklist line MR-05 (docs/port/map/a32-user-facing-features.md): "a saved level a model lacks
// runs at the nearest and Settings says so", for the meeting's installed agent on the Intelligence
// page (settings_models.py _refresh_meeting_thinking: "<Saved> runs at <Shown>" joined with " and "
// after " With this model, "). The cleanup page's half is pinned in
// tests/ui_personalize/test_cleanup_model_section.cpp.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>

#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "settings/testing/memory_store.h"
#include "ui_meeting/intelligence_page.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::agents::AgentModel;
using fv::agents::InstalledAgent;
using fv::agents::ScanResult;

namespace {

ScanResult found() {
    InstalledAgent claude;
    claude.id = "claude_code";
    claude.path = "C:/bin/claude_code.exe";
    claude.version = "2.1.281";
    claude.account = "Claude Team";
    claude.signed_in = true;
    return ScanResult{
        {"claude_code", claude}, {"codex", std::nullopt}, {"opencode_cli", std::nullopt}};
}

struct Page {
    fv::agent_picker::ScanSeam original = fv::agent_picker::scanSeam();
    fv::MemorySettingsStore settings;
    fv::AppServices services;
    std::optional<fv::AppContext> context;
    fv::SettingsPage container;
    std::optional<fv::IntelligencePage> page;

    explicit Page(const Json& values) : settings(values) {
        auto& seam = fv::agent_picker::scanSeam();
        seam.cached_agents = [] {
            return std::optional<ScanResult>(found());
        };
        seam.scan_installed_agents = [](bool) {
            return found();
        };
        seam.list_agent_models = [](const InstalledAgent&) {
            return std::vector<AgentModel>{{"", "Claude Code default"}};
        };
        services.settings = &settings;
        context.emplace(services);
        page.emplace(container, *context);
        container.resize(900, 1400);
        container.show();
        QApplication::processEvents();
    }
    ~Page() {
        page.reset();
        fv::agent_picker::waitForWorkers(std::chrono::seconds(5));
        fv::agent_picker::scanSeam() = std::move(original);
    }

    QString note() const {
        auto* label = page->findChild<fv::WrappedLabel*>(QStringLiteral("meetingBuiltinThinking"));
        REQUIRE(label != nullptr);
        return label->text();
    }
    QString shown(const char* name) const {
        auto* combo = page->findChild<QComboBox*>(QString::fromUtf8(name));
        REQUIRE(combo != nullptr);
        return combo->currentData().toString();
    }
};

} // namespace

TEST_CASE("a32 MR-05: a saved level the agent lacks runs at the nearest and the page says so",
          "[widget]") {
    // Claude Code offers Low to Max: a saved Ultra runs at Max, a saved Off at Low.
    Page rig(Json{{"meeting_llm_provider", "openrouter"},
                  {"meeting_llm_model", "deepseek/test-model"},
                  {"meeting_agent_core", "claude_code"},
                  {"meeting_live_thinking", "ultra"},
                  {"meeting_final_thinking", "off"}});
    CHECK(rig.shown("meeting_live_thinking") == QStringLiteral("max"));
    CHECK(rig.shown("meeting_final_thinking") == QStringLiteral("low"));
    CHECK(rig.note() ==
          QStringLiteral("Sent to Claude Code as its reasoning effort. Live passes run "
                         "every few minutes, so a lower level keeps cards quick. "
                         "With this model, Ultra runs at Max and Off runs at Low."));

    // Levels the agent takes say nothing more.
    Page plain(Json{{"meeting_agent_core", "claude_code"},
                    {"meeting_live_thinking", "low"},
                    {"meeting_final_thinking", "medium"}});
    CHECK_FALSE(plain.note().contains(QStringLiteral("runs at")));
}
