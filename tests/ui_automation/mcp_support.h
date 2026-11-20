#pragma once

// Settings > Automate > API & MCP over fakes (OpenWhisper's tests/test_mcp_settings_page.py
// make_page and FakeServer, and the fixtures of test_automation_ui_api_page.py):
// - an in-memory settings file;
// - `ScriptedMcpControl`: FakeServer, which starts and stops by script (start() answers
//   "starting", as the real listener does while its thread comes up) and records every start;
// - `copied`: what a copy put on the clipboard, never the real clipboard;
// - a private event bus, so listener counts are a test's own;
// - an io pool for the Clear button's work and the MCP Events counters, scripted.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include "agentapi/mcp_runtime.h"
#include "automation/event_bus.h"
#include "base/executor.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/live_automation_section.h"
#include "ui_automation/mcp_page.h"

#include "hooks_support.h"

namespace fvtest {

inline const std::string kMcpToken = "test-token-" + std::string(32, 'a');

// FakeServer: the listener the page reads.
// Affinity: GUI thread (a test's).
class ScriptedMcpControl final : public fv::agentapi::McpServerControl {
public:
    ScriptedMcpControl() = default;

    fv::agentapi::ServerStatus status() override { return current; }
    std::string token() override {
        if (tokenOverride) {
            return tokenOverride();
        }
        return current.state == "running" ? kMcpToken : std::string();
    }
    void start(int port, bool tailscale, std::function<bool()> = {}) override {
        starts.push_back(port);
        tailscaleStarts.push_back(tailscale);
        current = fv::agentapi::ServerStatus{"starting", "Starting MCP…", port, ""};
    }
    void stop(bool) override {
        current = fv::agentapi::ServerStatus{"stopped", "MCP is off.", current.port, ""};
    }

    fv::agentapi::ServerStatus current{"stopped", "MCP is off.", fv::agentapi::DEFAULT_PORT, ""};
    std::vector<int> starts;
    std::vector<bool> tailscaleStarts;
    // Set: token() answers this instead (a listener that lost its token).
    std::function<std::string()> tokenOverride;
};

// What a copy put on the clipboard, replacing the page's clipboard for the length of a test.
class CopyCapture {
public:
    CopyCapture() : previous_(fv::action_prompts::copyText()) {
        fv::action_prompts::copyText() = [this](const QString& text) {
            copied << text;
        };
    }
    ~CopyCapture() { fv::action_prompts::copyText() = previous_; }
    CopyCapture(const CopyCapture&) = delete;
    CopyCapture& operator=(const CopyCapture&) = delete;

    QString last() const { return copied.isEmpty() ? QString() : copied.last(); }
    QStringList copied;

private:
    fv::action_prompts::CopyText previous_;
};

// The process-wide event bus, replaced by one of the test's own.
class ScopedBus {
public:
    ScopedBus() : bus_(std::make_shared<fv::automation::EventBus>()) {
        previous_ = fv::automation::set_bus(bus_);
    }
    ~ScopedBus() {
        fv::automation::set_bus(previous_);
        bus_->close();
    }
    ScopedBus(const ScopedBus&) = delete;
    ScopedBus& operator=(const ScopedBus&) = delete;

    fv::automation::EventBus& get() { return *bus_; }
    std::shared_ptr<fv::automation::EventSubscription> listen(const std::string& label) {
        return bus_->subscribe([](const fv::automation::Event&) {}, std::nullopt, label);
    }

private:
    std::shared_ptr<fv::automation::EventBus> bus_;
    std::shared_ptr<fv::automation::EventBus> previous_;
};

// MCP Events' two calls, scripted: a count a test sets and a clear that may block, answer or
// fail.
class ScriptedEvents {
public:
    std::atomic<std::size_t> count{0};
    // Runs on the io pool.
    std::function<std::size_t()> onClear;
    std::atomic<int> clears{0};

    fv::LiveServices services(fv::Executor* io) {
        fv::LiveServices made;
        made.io = io;
        made.subscriptionCount = [this] {
            return count.load();
        };
        made.clearSubscriptions = [this] {
            ++clears;
            if (onClear) {
                return onClear();
            }
            const std::size_t gone = count.exchange(0);
            return gone;
        };
        return made;
    }
};

// One API & MCP page (or the whole view) over fakes, for the length of a test case. The settings
// file starts as `{}`, as make_page saves it.
class McpRig {
public:
    explicit McpRig(const Json& saved = Json::object()) : store(saved), io_("mcp-test-io", 2) {}
    ~McpRig() { io_.shutdown(std::chrono::seconds(5)); }
    McpRig(const McpRig&) = delete;
    McpRig& operator=(const McpRig&) = delete;

    // make_page: the local page alone.
    Held<fv::McpSettingsPage> makePage() {
        return Held<fv::McpSettingsPage>(new fv::McpSettingsPage(store, server));
    }
    // make_view: the page and the live section.
    Held<fv::McpSettingsView> makeView() {
        return Held<fv::McpSettingsView>(
            new fv::McpSettingsView(store, server, events.services(&io_)));
    }
    // The live section on its own.
    Held<fv::LiveAutomationSection> makeSection() {
        return Held<fv::LiveAutomationSection>(
            new fv::LiveAutomationSection(store, server, events.services(&io_)));
    }
    fv::ThreadPool& io() { return io_; }

    // The saved value of a key, null when it was never saved (SettingsManager.get).
    Json saved(std::string_view key) const { return store.value(key); }
    Json stored() const { return store.contents(); }

    fv::MemorySettingsStore store;
    ScriptedMcpControl server;
    ScriptedEvents events;
    CopyCapture clipboard;
    ScopedBus bus;

private:
    fv::ThreadPool io_;
};

// A page laid out in a scroll area of a width, as the Settings window lays it.
struct McpColumn {
    std::unique_ptr<QScrollArea> scroll;

    McpColumn(QWidget& content, bool stretchBelow = false)
        : scroll(std::make_unique<QScrollArea>()) {
        scroll->setWidgetResizable(true);
        if (stretchBelow) {
            // A stretch below the page used to compress buttons below their text height.
            auto* wrapper = new QWidget;
            auto* layout = new QVBoxLayout(wrapper);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(&content);
            layout->addStretch();
            scroll->setWidget(wrapper);
        } else {
            scroll->setWidget(&content);
        }
    }
    void resize(int width, int height = 900) {
        scroll->resize(width, height);
        scroll->show();
        settle();
    }
    // Wrapping and reflow propagate through nested layouts over several events.
    static void settle(int times = 8) { pump(times); }
};

} // namespace fvtest
