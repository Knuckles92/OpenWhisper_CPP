#pragma once

// AI cleanup and Learned rules inside a real Settings window over fakes (OpenWhisper's
// settings_dialogs and the patches of test_settings_models.py and test_rule_composer.py), with
// what the pages reach outside the window:
// - a two-thread pool standing in for the io and jobs strands, so the pages' blocking work runs
//   off the GUI thread as in the app and its results arrive with the event loop;
// - key status through llm::credential_status over an in-memory credential store (keys added
//   with key()), no environment and no .env;
// - a scripted catalog (`catalog`: the models it lists, or the error it fails with, and every
//   request), local-server detection (`servers`) and rule polish (`polish`);
// - rule dictation over audio::FakeBackend with a short post-roll, a fake RuleTranscriber, and
//   the take saved in a folder of the rig's own;
// - the confirmations answered `answer` and recorded in `asked`, the prompt editor, the endpoint
//   dialog and the rule review answered by the test;
// - the agent picker's scan and model lists (agent_picker::scanSeam) answered from `agentScan`:
//   Claude Code, Codex and OpenCode installed and signed in, each with a short catalog.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QMessageBox>
#include <QStringList>
#include <QTest>

#include "audio/testing/fake_backend.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "llm/credentials.h"
#include "recorder/recorder.h"
#include "styles_commands_support.h"
#include "testkit/testkit.h"
#include "text/cleanup_levels.h"
#include "ui_personalize/cleanup_model_section.h"
#include "ui_personalize/cleanup_page.h"
#include "ui_personalize/cleanup_rules_section.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/rule_dictation.h"
#include "ui_personalize/rules_page.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/agent_tile.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/wrapped_label.h"

namespace fvtest {

class FakeRuleTranscriber final : public fv::RuleTranscriber {
public:
    std::string transcribeClip(const std::filesystem::path& audio) override {
        ++calls;
        sawAudio = std::filesystem::exists(audio);
        return answer;
    }
    std::string answer = "capitalize kubernetes";
    std::atomic<int> calls{0};
    std::atomic<bool> sawAudio{false};
};

// The agent picker's scan and model lists for the rig's lifetime; the real seam comes back once
// the picker's workers are done.
struct AgentScanFake {
    struct State {
        std::mutex mutex;
        fv::agents::ScanResult found;
        std::map<std::string, std::vector<fv::agents::AgentModel>> models;
        int scans = 0;
        std::vector<std::string> listed;
    };

    static fv::agents::InstalledAgent ready(std::string id, std::string version,
                                            std::string account) {
        fv::agents::InstalledAgent agent;
        agent.id = std::move(id);
        agent.path = "C:/bin/" + agent.id + ".exe";
        agent.version = std::move(version);
        agent.account = std::move(account);
        agent.signed_in = true;
        return agent;
    }

    AgentScanFake() {
        state->found = {{"claude_code", ready("claude_code", "2.1.281", "Claude Team")},
                        {"codex", ready("codex", "0.158.0", "ChatGPT")},
                        {"opencode_cli", ready("opencode_cli", "1.2.3", "")}};
        state->models = {
            {"claude_code",
             {{"", "Claude Code default"}, {"haiku", "Haiku (fastest)"}, {"sonnet", "Sonnet"}}},
            {"codex", {{"", "Codex default"}, {"gpt-a", "GPT A"}, {"gpt-b", "GPT B"}}},
            {"opencode_cli", {{"", "OpenCode default"}, {"anthropic/claude-sonnet", "Sonnet"}}}};
        auto& seam = fv::agent_picker::scanSeam();
        seam.cached_agents = [state = state] {
            std::lock_guard lock(state->mutex);
            return std::optional<fv::agents::ScanResult>(state->found);
        };
        seam.scan_installed_agents = [state = state](bool) {
            std::lock_guard lock(state->mutex);
            ++state->scans;
            return state->found;
        };
        seam.list_agent_models = [state = state](const fv::agents::InstalledAgent& agent) {
            std::lock_guard lock(state->mutex);
            state->listed.push_back(agent.id);
            return state->models[agent.id];
        };
    }
    ~AgentScanFake() {
        fv::agent_picker::waitForWorkers(std::chrono::seconds(5));
        fv::agent_picker::scanSeam() = std::move(original);
    }
    AgentScanFake(const AgentScanFake&) = delete;
    AgentScanFake& operator=(const AgentScanFake&) = delete;

    fv::agent_picker::ScanSeam original = fv::agent_picker::scanSeam();
    std::shared_ptr<State> state = std::make_shared<State>();
};

// What outlives the window: built before it and gone after it.
struct CleanupFakes {
    CleanupFakes()
        : keys(std::make_shared<fv::MemoryCredentialBackend>()), credentials(keys, fv::identity()),
          dotenv(folder.path() / "no-such.env"),
          microphone(std::make_shared<fv::audio::testing::FakeBackend>()),
          transcriber(std::make_shared<FakeRuleTranscriber>()) {
        microphone->set_layout({fv::audio::testing::FakeHostApi{"MME", 0, -1}},
                               {fv::audio::testing::FakeDevice{"USB microphone", 0, 1, 0, 44100.0}},
                               0);
    }

    // The catalog's answer and the requests it got.
    struct Catalog {
        std::mutex mutex;
        std::vector<std::string> models;
        std::string error;
        std::vector<std::pair<std::string, std::string>> requests;
    };
    struct Servers {
        std::mutex mutex;
        std::vector<std::pair<std::string, std::string>> found;
        std::vector<std::vector<std::string>> probes;
    };
    struct Polish {
        std::mutex mutex;
        std::string polished = "Always write Kubernetes with a capital K.";
        // Set: the polish fails with this error and keeps the words.
        std::optional<std::string> error;
        std::vector<std::string> asked;
    };

    fv::testkit::TempDataRoot folder;
    std::shared_ptr<fv::MemoryCredentialBackend> keys;
    fv::CredentialStore credentials;
    fv::MapEnvReader environment;
    fv::DotEnv dotenv;
    std::shared_ptr<fv::audio::testing::FakeBackend> microphone;
    std::shared_ptr<FakeRuleTranscriber> transcriber;
    std::shared_ptr<Catalog> catalog = std::make_shared<Catalog>();
    std::shared_ptr<Servers> servers = std::make_shared<Servers>();
    std::shared_ptr<Polish> polish = std::make_shared<Polish>();
    std::vector<std::vector<fv::audio::DeviceKey>> recorderPriorities;
    AgentScanFake agentScan;
    fv::ThreadPool pool{"fv-test-cleanup", 2};
};

class CleanupRig : private CleanupFakes, public PersonalizeRig {
public:
    // `withTranscriber` false: no RuleTranscriber is provided, as before the controller provides
    // one.
    explicit CleanupRig(const fv::Json& saved = fv::Json::object(), bool withTranscriber = true)
        : PersonalizeRig(saved) {
        fv::CleanupServices& services = cleanup().services();
        services.ui = &context().ui();
        services.io = &pool;
        services.jobs = &pool;
        services.listModels = [catalog = catalog](const std::string& provider,
                                                  const std::string& sort, const fv::Json&) {
            std::lock_guard lock(catalog->mutex);
            catalog->requests.emplace_back(provider, sort);
            if (!catalog->error.empty()) {
                throw std::runtime_error(catalog->error);
            }
            return catalog->models;
        };
        services.credentialStatus = [this](const fv::llm::TextLLMProfile& profile) {
            fv::llm::KeySources sources{credentials, environment, dotenv};
            return fv::llm::credential_status(profile, sources);
        };
        services.detectServers = [servers = servers](const std::vector<std::string>& urls) {
            std::lock_guard lock(servers->mutex);
            servers->probes.push_back(urls);
            return servers->found;
        };
        services.polishRule = [polish = polish](const std::string& raw, const std::string&,
                                                const std::string&, const std::string&) {
            std::lock_guard lock(polish->mutex);
            polish->asked.push_back(raw);
            if (polish->error) {
                return std::pair<std::string, std::optional<std::string>>(raw, polish->error);
            }
            return std::pair<std::string, std::optional<std::string>>(polish->polished,
                                                                      std::nullopt);
        };
        services.makeRecorder = [this](const std::vector<fv::audio::DeviceKey>& priority,
                                       const std::filesystem::path& output) {
            recorderPriorities.push_back(priority);
            fv::RecorderOptions options;
            options.output_file = output;
            options.device_priority = priority;
            options.timings.post_roll = std::chrono::milliseconds(50);
            return std::make_shared<fv::Recorder>(*microphone, context().paths(),
                                                  std::move(options));
        };
        services.meetingActive = [this] {
            return meetingActive;
        };
        if (withTranscriber) {
            context().provide<fv::RuleTranscriber>(transcriber);
        }
        cleanup().question = [this](QWidget*, const QString& title, const QString& text,
                                    QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            asked << title + QStringLiteral(": ") + text;
            return answer;
        };
    }
    ~CleanupRig() {
        // The pool's work posts to the window's GUI executor, which goes with the window.
        pool.shutdown(std::chrono::seconds(5));
    }
    CleanupRig(const CleanupRig&) = delete;
    CleanupRig& operator=(const CleanupRig&) = delete;

    using CleanupFakes::agentScan;
    using CleanupFakes::catalog;
    using CleanupFakes::microphone;
    using CleanupFakes::polish;
    using CleanupFakes::recorderPriorities;
    using CleanupFakes::servers;
    using CleanupFakes::transcriber;

    fv::CleanupSettings& cleanup() { return context().service<fv::CleanupSettings>(); }

    // A saved key, found by variable name.
    void key(const std::string& env, const std::string& value) {
        keys->set(credentials.service(), env, value);
    }

    fv::CleanupPage& cleanupPage() {
        window();
        fv::CleanupPage& made = page<fv::CleanupPage>(dest::cleanup);
        guardPage(made);
        return made;
    }
    // The page built without selecting it (ensure_page).
    fv::CleanupPage& builtCleanupPage() {
        window().ensurePage(dest::cleanup);
        auto* made = dynamic_cast<fv::CleanupPage*>(window().pageContent(dest::cleanup));
        REQUIRE(made != nullptr);
        guardPage(*made);
        return *made;
    }
    fv::RulesPage& rulesPage() {
        window();
        fv::RulesPage& made = page<fv::RulesPage>(dest::cleanup_rules);
        guardRules(made);
        return made;
    }
    fv::RulesPage& builtRulesPage() {
        window().ensurePage(dest::cleanup_rules);
        auto* made = dynamic_cast<fv::RulesPage*>(window().pageContent(dest::cleanup_rules));
        REQUIRE(made != nullptr);
        guardRules(*made);
        return *made;
    }
    fv::CleanupRulesSection& rules() { return *rulesPage().section(); }
    fv::CleanupModelSection& model() { return *builtCleanupPage().modelSection(); }
    fv::TextModelPicker& picker() { return *model().picker(); }
    fv::AgentPicker& agents() { return *model().agentPicker(); }
    // Clicks an agent's tile (or legacy_builtin_agent_id's), as a person would.
    void clickTile(const QString& id) {
        fv::AgentTile* tile = agents().tile(id);
        REQUIRE(tile != nullptr);
        QTest::mouseClick(tile, Qt::LeftButton, {}, QPoint(20, 20));
    }
    // Picks a model in the agent's model row, as a person would; "" is the agent's default.
    // Waits for a list that loads in the background (OpenCode).
    void chooseAgentModel(const QString& value) {
        QComboBox* combo = agents().modelCombo();
        REQUIRE(pumpUntil([&] {
            combo = agents().modelCombo();
            return combo->isEnabled() && combo->findData(value) >= 0;
        }));
        Q_EMIT combo->activated(combo->findData(value));
    }
    // The value the agent's model row shows.
    QString shownAgentModel() { return agents().modelCombo()->currentData().toString(); }
    // _assert_agent_card: one Cleanup model card holding the agent's model row and the thinking
    // level, the endpoint picker out of it, and both texts naming the agent.
    void checkAgentCard(std::string_view agentId) {
        fv::CleanupModelSection& tile = model();
        CHECK_FALSE(tile.isHidden());
        CHECK(tile.isAncestorOf(agents().modelCard()));
        CHECK(agents().modelCard()->isVisibleTo(&tile));
        CHECK_FALSE(picker().isVisibleTo(&tile));
        CHECK(tile.reasoningCombo()->isEnabled());
        const QString name =
            QString::fromUtf8(fv::agents::agent_spec(agentId).name.data(),
                              static_cast<qsizetype>(fv::agents::agent_spec(agentId).name.size()));
        CHECK(tile.descriptionLabel()->text().contains(name));
        CHECK(tile.reasoningNote()->text().contains(QStringLiteral("Sent to ") + name));
    }

    // The level the bar shows.
    std::string selected() {
        fv::SegmentedBar* bar = builtCleanupPage().levelBar();
        return std::string(
            fv::text::cleanup_levels::SEGMENTS[static_cast<std::size_t>(bar->currentIndex())]
                .value);
    }
    // Clicks the segment of a level, as a person would, then lets the 0 ms resync run.
    void clickLevel(std::string_view level) {
        fv::SegmentedBar* bar = builtCleanupPage().levelBar();
        bar->buttons()[fv::text::cleanup_levels::segment_index(level)]->click();
        processEvents(3);
    }

    // Home's Cleanup level combo.
    QComboBox* homeLevelCombo() {
        for (QComboBox* combo : home().findChildren<QComboBox*>()) {
            if (combo->accessibleName() == QStringLiteral("Cleanup level")) {
                return combo;
            }
        }
        FAIL("Home has no Cleanup level row");
        return nullptr;
    }

    // Turns the event loop until the pool's work has come back.
    static void settle(int ms = 100) { wait(ms); }

    bool meetingActive = false;
    // What the prompt editor was opened with, and what it answers (nullopt: cancelled).
    QStringList editorOpened;
    std::optional<QString> editorAnswer;
    // The review dialog's openings, and its answer.
    std::vector<fv::RuleReview> reviews;
    std::optional<QString> reviewAnswer;
    // The endpoint dialog's openings (nullopt: Add) and its answer.
    std::vector<std::optional<fv::llm::TextLLMProfile>> endpointDialogs;
    std::optional<fv::TextEndpointDialog::Payload> endpointAnswer;

private:
    void guardPage(fv::CleanupPage& made) {
        made.editPrompt = [this](const QString& prompt, QWidget*) {
            editorOpened << prompt;
            return editorAnswer;
        };
        fv::CleanupModelSection& section = *made.modelSection();
        section.editEndpoint = [this](std::optional<fv::llm::TextLLMProfile> profile, QWidget*) {
            endpointDialogs.push_back(std::move(profile));
            return endpointAnswer;
        };
        section.question = [this](QWidget*, const QString& title, const QString& text,
                                  QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            asked << title + QStringLiteral(": ") + text;
            return answer;
        };
    }
    void guardRules(fv::RulesPage& made) {
        fv::CleanupRulesSection& section = *made.section();
        section.setDictationPath(CleanupFakes::folder.path() / "rule_dictation.wav");
        section.reviewRule = [this](const fv::RuleReview& review, QWidget*) {
            reviews.push_back(review);
            return reviewAnswer;
        };
    }
};

} // namespace fvtest
