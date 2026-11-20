// The picker of who runs AI insights or AI cleanup (ui_qt/widgets/agent_picker.py), ported from
// tests/test_agent_picker.py. The picker never runs an agent here: every test replaces the scan
// seam (agent_picker::scanSeam) the way OpenWhisper's conftest and patches do, and the tests of the
// default seam and of the lookup relay use FakeInstalledAgents, whose launcher starts nothing.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QWidget>

#include "agents/installed_agents.h"
#include "agents/testing/fake_installed_agents.h"
#include "base/identity.h"
#include "ui_widgets/agent_lookup.h"
#include "ui_widgets/agent_mark.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/agent_tile.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/wrapped_label.h"

namespace agents = fv::agents;
namespace picker = fv::agent_picker;
using agents::AgentModel;
using agents::InstalledAgent;
using agents::ScanResult;
using Catch::Approx;
using fv::AgentPicker;
using fv::AgentTile;

namespace {

const QString kClaudeCode = QStringLiteral("claude_code");
const QString kCodex = QStringLiteral("codex");
const QString kOpenCode = QStringLiteral("opencode_cli");

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString builtin() {
    return qs(fv::legacy_builtin_agent_id);
}

InstalledAgent agent(std::string id, std::string path, std::string version,
                     std::string account = "", std::optional<bool> signedIn = std::nullopt,
                     std::string problem = "") {
    InstalledAgent result;
    result.id = std::move(id);
    result.path = std::move(path);
    result.version = std::move(version);
    result.account = std::move(account);
    result.signed_in = signedIn;
    result.problem = std::move(problem);
    return result;
}

const InstalledAgent kClaude =
    agent("claude_code", "C:/bin/claude.exe", "2.1.281", "Claude Team", true);
const InstalledAgent kCodexAgent = agent("codex", "C:/bin/codex.exe", "0.158.0", "ChatGPT", true);
const InstalledAgent kOpenCodeAgent = agent("opencode_cli", "C:/bin/opencode.exe", "2.1.0");

ScanResult foundTwo() {
    return {{"claude_code", kClaude}, {"codex", kCodexAgent}, {"opencode_cli", std::nullopt}};
}

std::vector<AgentModel> models(const InstalledAgent& agent) {
    if (agent.id == "claude_code") {
        return {
            {"", "Claude Code default (opus)"}, {"haiku", "Haiku (fastest)"}, {"sonnet", "Sonnet"}};
    }
    if (agent.id == "codex") {
        return {{"", "Codex default"}};
    }
    throw std::out_of_range(agent.id);
}

bool pumpUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeout.count()) {
        QCoreApplication::processEvents();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

void pumpFor(std::chrono::milliseconds time) {
    pumpUntil([] { return false; }, time);
}

// OpenWhisper's conftest for Qt tests: the scan finds nothing, there is no cached scan, and an
// agent lists only its default. Restored, after every worker finished, when the test ends.
class SeamScope {
public:
    SeamScope() {
        picker::ScanSeam& seam = picker::scanSeam();
        seam.scan_installed_agents = [](bool) {
            ScanResult nothing;
            for (const std::string_view id : agents::agent_order()) {
                nothing[std::string(id)] = std::nullopt;
            }
            return nothing;
        };
        seam.cached_agents = [] {
            return std::optional<ScanResult>();
        };
        seam.list_agent_models = [](const InstalledAgent& agent) {
            return std::vector<AgentModel>{
                {"", std::string(agents::agent_spec(agent.id).name) + " default"}};
        };
    }
    ~SeamScope() {
        picker::waitForWorkers(std::chrono::seconds(10));
        QCoreApplication::processEvents();
        picker::resetScanSeam();
    }
    SeamScope(const SeamScope&) = delete;
    SeamScope& operator=(const SeamScope&) = delete;

    picker::ScanSeam& seam() { return picker::scanSeam(); }
    void cache(std::optional<ScanResult> scan) {
        seam().cached_agents = [scan] {
            return scan;
        };
    }
};

// A gate a worker waits at, up to 5 s, as threading.Event().wait(5) does.
class Gate {
public:
    void open() {
        std::lock_guard lock(mutex_);
        open_ = true;
        opened_.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        opened_.wait_for(lock, std::chrono::seconds(5), [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable opened_;
    bool open_ = false;
};

std::vector<AgentTile*> agentTiles(const AgentPicker& p) {
    return {p.tile(kClaudeCode), p.tile(kCodex), p.tile(kOpenCode)};
}

} // namespace

// ---- TestTileCopy (golden vectors cover the same functions over more inputs) ----

TEST_CASE("found agent shows version and account", "[test_agent_picker]") {
    // from tests/test_agent_picker.py::TestTileCopy::test_found_agent_shows_version_and_account
    const auto state = picker::describe_agent("claude_code", kClaude);
    CHECK(state.tone == "found");
    CHECK(state.pill == "Found");
    CHECK(state.detail == "2.1.281 · Claude Team");
    CHECK((state.selectable && state.installed));
}

TEST_CASE("missing agent is not selectable", "[test_agent_picker]") {
    // from tests/test_agent_picker.py::TestTileCopy::test_missing_agent_is_not_selectable
    const auto state = picker::describe_agent("opencode_cli", std::nullopt);
    CHECK(state.tone == "missing");
    CHECK(state.pill == "Not installed");
    CHECK(state.detail.empty());
    CHECK_FALSE(state.selectable);
    CHECK_FALSE(state.installed);
}

TEST_CASE("problem and sign in show amber with the fix", "[test_agent_picker]") {
    // from
    // tests/test_agent_picker.py::TestTileCopy::test_problem_and_sign_in_show_amber_with_the_fix
    const auto old = agent("codex", "codex", "0.100.0", "", std::nullopt,
                           std::string(fv::identity().product) +
                               " needs Codex 0.120.0 or newer. Run `codex x`.");
    auto state = picker::describe_agent("codex", old);
    CHECK(state.tone == "warning");
    CHECK(state.pill == "Update needed");
    CHECK(state.detail.find("“codex x”") != std::string::npos);
    CHECK(state.detail.find('`') == std::string::npos);
    CHECK_FALSE(state.selectable);
    CHECK(state.installed);

    const auto signedOut = agent("codex", "codex", "0.158.0", "", false);
    state = picker::describe_agent("codex", signedOut);
    CHECK(state.tone == "warning");
    CHECK(state.pill == "Sign in needed");
    CHECK(state.detail.find("codex login") != std::string::npos);
}

TEST_CASE("unknown sign in is still usable", "[test_agent_picker]") {
    // from tests/test_agent_picker.py::TestTileCopy::test_unknown_sign_in_is_still_usable
    const auto state = picker::describe_agent("opencode_cli", kOpenCodeAgent);
    CHECK(state.tone == "found");
    CHECK(state.selectable);
    CHECK(state.detail == "2.1.0");
}

TEST_CASE("count line", "[test_agent_picker]") {
    // from tests/test_agent_picker.py::TestTileCopy::test_count_line
    CHECK(picker::found_count_text(foundTwo()) == "Found 2 coding agents on this computer.");
    CHECK(picker::found_count_text({{"codex", kCodexAgent}}) ==
          "Found 1 coding agent on this computer.");
    const std::string empty = picker::found_count_text({});
    CHECK(empty.starts_with("No coding agents found"));
    CHECK(empty.find("Pi") != std::string::npos);
    CHECK(empty.find("component is installed") != std::string::npos);
    CHECK(empty.find(std::string(fv::identity().product) + "'s built-in") == std::string::npos);
    CHECK(empty.find(std::string(fv::legacy_identity().product) + "'s built-in") ==
          std::string::npos);
}

TEST_CASE("usage caption names the sign in and the limits", "[test_agent_picker]") {
    // from
    // tests/test_agent_picker.py::TestTileCopy::test_usage_caption_names_the_sign_in_and_the_limits
    const std::string text = picker::usage_caption(kClaude);
    CHECK(text.starts_with("Runs through your Claude Code sign-in (Claude Team)."));
    CHECK(text.find("counts toward that plan's usage") != std::string::npos);
    CHECK(text.find("no files, no shell") != std::string::npos);
    const auto key = agent("claude_code", "claude", "2.1.0", "API key", true);
    CHECK(picker::usage_caption(key).find("billed to that key") != std::string::npos);
    const auto bedrock = agent("claude_code", "claude", "2.1.0", "Amazon Bedrock", true);
    CHECK(picker::usage_caption(bedrock).find("that account's usage") != std::string::npos);
    CHECK(picker::usage_caption(kOpenCodeAgent).find("providers you signed in to in OpenCode") !=
          std::string::npos);
}

TEST_CASE("blocked notice", "[test_agent_picker]") {
    // from tests/test_agent_picker.py::TestTileCopy::test_blocked_notice
    CHECK(picker::blocked_notice("opencode_cli", std::nullopt).find("until it is installed") !=
          std::string::npos);
    CHECK(picker::blocked_notice("claude_code", kClaude).empty());
    const auto signedOut = agent("codex", "codex", "0.158.0", "", false);
    CHECK(picker::blocked_notice("codex", signedOut).find("Sign in and choose Look again") !=
          std::string::npos);
}

TEST_CASE("command text", "[test_agent_picker]") {
    // from tests/test_agent_picker.py::TestTileCopy::test_command_text
    CHECK(picker::command_text("Run `claude update`.") == "Run “claude update”.");
}

// ---- TestScanning ----

TEST_CASE("cleanup picker labels usage and narrow layout", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestScanning::test_cleanup_picker_labels_usage_and_narrow_layout
    SeamScope scope;
    scope.cache(foundTwo());
    AgentPicker p(QStringLiteral("AI cleanup"));
    p.setChoice(kClaudeCode);
    p.resize(430, 750);
    p.show();
    QCoreApplication::processEvents();
    const QString usage = p.usageLabel()->text();
    CHECK(usage.contains(QStringLiteral("cleanup request")));
    CHECK(usage.contains(QStringLiteral("no tools, files, or shell")));
    CHECK_FALSE(usage.contains(QStringLiteral("meeting")));
    CHECK(p.tile(kClaudeCode)->accessibleDescription().contains(QStringLiteral("AI cleanup")));
    CHECK(p.tile(builtin())->name() == QStringLiteral("API / local model"));
    for (AgentTile* tile : p.tiles()) {
        CHECK(tile->x() + tile->cardRect().right() <= p.grid()->width());
    }
    p.setChoice(builtin());
    CHECK(p.tile(builtin())->name() == QStringLiteral("API / local model"));
    p.close();
}

TEST_CASE("cached scan shows at once without animating", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestScanning::test_cached_scan_shows_at_once_without_animating
    SeamScope scope;
    scope.cache(foundTwo());
    int scans = 0;
    scope.seam().scan_installed_agents = [&scans](bool) {
        ++scans;
        return ScanResult();
    };
    AgentPicker p;
    p.ensureScanned();
    CHECK(scans == 0);
    CHECK_FALSE(p.isScanning());
    const std::vector<std::pair<QString, std::string>> tones{
        {kClaudeCode, "found"}, {kCodex, "found"}, {kOpenCode, "missing"}};
    for (const auto& [id, tone] : tones) {
        AgentTile* tile = p.tile(id);
        CHECK(tile->state().tone == tone);
        CHECK(tile->arrival() == 1.0);
        CHECK_FALSE(tile->arrivalPending());
        CHECK(tile->shimmer() == 0.0);
    }
    CHECK(p.tile(kClaudeCode)->mark()->warmth() == 1.0);
    CHECK(p.countLabel()->text() == QStringLiteral("Found 2 coding agents on this computer."));
}

TEST_CASE("fresh scan shimmers until the result arrives", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestScanning::test_fresh_scan_shimmers_until_the_result_arrives
    SeamScope scope;
    auto gate = std::make_shared<Gate>();
    auto calls = std::make_shared<std::vector<bool>>();
    auto callsMutex = std::make_shared<std::mutex>();
    scope.seam().scan_installed_agents = [gate, calls, callsMutex](bool refresh) {
        {
            std::lock_guard lock(*callsMutex);
            calls->push_back(refresh);
        }
        gate->wait();
        return foundTwo();
    };
    AgentPicker p;
    p.ensureScanned();
    CHECK(p.isScanning());
    CHECK_FALSE(p.lookAgainButton()->isEnabled());
    CHECK(p.countLabel()->text().contains(QStringLiteral("Looking for coding agents")));
    for (AgentTile* tile : agentTiles(p)) {
        CHECK(tile->scanning());
        CHECK(tile->shimmer() == 1.0);
        CHECK(tile->pill()->text() == QStringLiteral(u"Looking…"));
    }
    // The built-in tile has nothing to look for.
    CHECK_FALSE(p.tile(builtin())->scanning());
    p.ensureScanned(); // a second open does not start another
    gate->open();
    CHECK(pumpUntil([&] { return !p.isScanning(); }));
    {
        std::lock_guard lock(*callsMutex);
        CHECK(*calls == std::vector<bool>{false});
    }
    CHECK(p.lookAgainButton()->isEnabled());
    CHECK_FALSE(p.tile(kClaudeCode)->scanning());
    CHECK(p.agent(kClaudeCode) == std::optional<InstalledAgent>(kClaude));
}

TEST_CASE("fresh result lights tiles up one after another", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestScanning::test_fresh_result_lights_tiles_up_one_after_another
    SeamScope scope;
    AgentPicker p;
    p.startScan(false);
    p.onScanDone(p.generation(), foundTwo());
    AgentTile* first = p.tile(kClaudeCode);
    AgentTile* second = p.tile(kCodex);
    AgentTile* third = p.tile(kOpenCode);
    CHECK(first->state().tone == "found");
    CHECK_FALSE(first->arrivalPending());
    CHECK(first->arrival() < 1.0);
    // The shimmer stops for every tile the moment the scan is back.
    CHECK_FALSE((first->scanning() || second->scanning() || third->scanning()));
    CHECK(second->arrivalPending());
    CHECK(third->arrivalPending());
    CHECK(second->arrivalTimer()->interval() == picker::kStaggerMs);
    CHECK(third->arrivalTimer()->interval() == 2 * picker::kStaggerMs);
    CHECK(pumpUntil([&] { return third->arrival() == 1.0 && !third->arrivalPending(); }));
    CHECK(second->state().tone == "found");
    CHECK(second->mark()->warmth() == 1.0);
    CHECK(third->state().tone == "missing");
    CHECK(third->mark()->missing() == 1.0);
    CHECK(first->pill()->text() == QStringLiteral("Found"));
}

TEST_CASE("look again scans with refresh", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestScanning::test_look_again_scans_with_refresh
    SeamScope scope;
    scope.cache(ScanResult());
    auto calls = std::make_shared<std::vector<bool>>();
    auto callsMutex = std::make_shared<std::mutex>();
    scope.seam().scan_installed_agents = [calls, callsMutex](bool refresh) {
        std::lock_guard lock(*callsMutex);
        calls->push_back(refresh);
        return foundTwo();
    };
    AgentPicker p;
    p.lookAgainButton()->click();
    CHECK(pumpUntil([&] { return !p.isScanning(); }));
    {
        std::lock_guard lock(*callsMutex);
        CHECK(*calls == std::vector<bool>{true});
    }
    CHECK(pumpUntil([&] { return !p.tile(kCodex)->arrivalPending(); }));
    CHECK(p.tile(kCodex)->state().tone == "found");
}

TEST_CASE("a stale scan result is ignored", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestScanning::test_a_stale_scan_result_is_ignored
    SeamScope scope;
    AgentPicker p;
    p.startScan(false);
    const std::uint64_t stale = p.generation();
    p.startScan(true);
    CHECK(p.generation() != stale);
    p.onScanDone(stale, foundTwo());
    CHECK(p.isScanning());
    CHECK_FALSE(p.agents().has_value());
}

TEST_CASE("a picker closed mid scan is not held by the worker", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestScanning::test_a_picker_closed_mid_scan_is_not_held_by_the_worker
    const bool parented = GENERATE(false, true);
    SeamScope scope;
    scope.cache(std::nullopt);
    auto gate = std::make_shared<Gate>();
    scope.seam().scan_installed_agents = [gate](bool) {
        gate->wait();
        return foundTwo();
    };
    std::unique_ptr<QWidget> parent = parented ? std::make_unique<QWidget>() : nullptr;
    auto* p = new AgentPicker(QStringLiteral("AI insights"), true, parent.get());
    p->startScan(false);
    CHECK(picker::runningWorkers() >= 1);
    QPointer<AgentPicker> ref(p);
    if (parent) {
        parent.reset();
    } else {
        delete p;
    }
    CHECK(ref.isNull());
    gate->open();
    CHECK(picker::waitForWorkers(std::chrono::seconds(5)));
    // The result reaches no picker: the relay delivers only to the pickers that still exist.
    QCoreApplication::processEvents();
}

TEST_CASE("a failing scan reads as nothing found", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestScanning::test_a_failing_scan_reads_as_nothing_found
    SeamScope scope;
    scope.seam().scan_installed_agents = [](bool) -> ScanResult {
        throw std::runtime_error("x");
    };
    AgentPicker p;
    p.ensureScanned();
    CHECK(pumpUntil([&] { return !p.isScanning(); }));
    const std::vector<AgentTile*> tiles = agentTiles(p);
    CHECK(pumpUntil([&] {
        return std::none_of(tiles.begin(), tiles.end(),
                            [](AgentTile* tile) { return tile->arrivalPending(); });
    }));
    for (AgentTile* tile : tiles) {
        CHECK(tile->state().tone == "missing");
    }
    CHECK(p.countLabel()->text().startsWith(QStringLiteral("No coding agents found")));
}

// ---- TestChoosing ----

namespace {

// TestChoosing's fixture: the cached scan found Claude Code and Codex.
struct Choosing {
    SeamScope scope;
    std::unique_ptr<AgentPicker> p;

    Choosing() {
        scope.cache(foundTwo());
        scope.seam().list_agent_models = models;
        p = std::make_unique<AgentPicker>();
        p->resize(900, 400);
    }
};

// Receives the URLs QDesktopServices would open, so no browser starts.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    std::vector<QUrl> opened;

public Q_SLOTS:
    void open(const QUrl& url) { opened.push_back(url); }
};

} // namespace

TEST_CASE("native tile names pi and does not use the app icon", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestChoosing::test_native_tile_names_pi_and_does_not_use_the_app_icon
    const QString core = GENERATE(QStringLiteral("pi"), QStringLiteral("direct"));
    Choosing fixture;
    fixture.p->setChoice(core);
    AgentTile* tile = fixture.p->tile(builtin());
    CHECK(tile->name() == QStringLiteral("Pi"));
    CHECK(tile->nameLabel()->text() == QStringLiteral("Pi"));
    CHECK(tile->accessibleName() == QStringLiteral("Pi, Built in"));
    CHECK(tile->mark()->monogram() == QStringLiteral(u"π"));
    // mark._pixmap is None: the mark paints exactly as a monogram-only mark of its colour does.
    // A sibling in the same tile, so palette, font and background are the mark's own.
    fv::AgentMark* mark = tile->mark();
    mark->resize(mark->sizeHint());
    fv::AgentMark monogramOnly(QStringLiteral(u"π"), mark->accent(), QPixmap(), tile);
    monogramOnly.setGeometry(mark->geometry());
    CHECK(mark->grab().toImage() == monogramOnly.grab().toImage());
    CHECK(tile->detailLabel()->text().contains(QStringLiteral("Your API key")));
    const QString description = tile->accessibleDescription();
    CHECK_FALSE(description.contains(qs(fv::legacy_identity().product)));
    CHECK_FALSE(description.contains(qs(fv::identity().product)));
}

TEST_CASE("packaged opencode is not mislabeled as pi", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestChoosing::test_packaged_opencode_is_not_mislabeled_as_pi
    Choosing fixture;
    fixture.p->setChoice(QStringLiteral("opencode"));
    AgentTile* tile = fixture.p->tile(builtin());
    CHECK(tile->nameLabel()->text() == QStringLiteral("OpenCode SDK"));
    CHECK(tile->accessibleName() == QStringLiteral("OpenCode SDK, Built in"));
    CHECK(tile->mark()->monogram() == QStringLiteral("OC"));
    fixture.p->setChoice(kCodex);
    CHECK(tile->nameLabel()->text() == QStringLiteral("OpenCode SDK"));
    fixture.p->setChoice(QStringLiteral("pi"));
    CHECK(tile->nameLabel()->text() == QStringLiteral("Pi"));
    CHECK(tile->mark()->monogram() == QStringLiteral(u"π"));
}

TEST_CASE("found tile click requests that agent", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestChoosing::test_found_tile_click_requests_that_agent
    Choosing fixture;
    QStringList requested;
    QObject::connect(fixture.p.get(), &AgentPicker::choiceRequested,
                     [&](const QString& id) { requested << id; });
    QTest::mouseClick(fixture.p->tile(kCodex), Qt::LeftButton, {}, QPoint(20, 20));
    CHECK(requested == QStringList{kCodex});
}

TEST_CASE("missing tile click does nothing", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestChoosing::test_missing_tile_click_does_nothing
    Choosing fixture;
    QStringList requested;
    QObject::connect(fixture.p.get(), &AgentPicker::choiceRequested,
                     [&](const QString& id) { requested << id; });
    AgentTile* tile = fixture.p->tile(kOpenCode);
    QTest::mouseClick(tile, Qt::LeftButton, {}, QPoint(20, 20));
    CHECK(requested.isEmpty());
    CHECK(tile->focusPolicy() == Qt::NoFocus);
}

TEST_CASE("space on a found tile requests it", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestChoosing::test_space_on_a_found_tile_requests_it
    Choosing fixture;
    QStringList requested;
    QObject::connect(fixture.p.get(), &AgentPicker::choiceRequested,
                     [&](const QString& id) { requested << id; });
    QTest::keyClick(fixture.p->tile(kClaudeCode), Qt::Key_Space);
    CHECK(requested == QStringList{kClaudeCode});
}

TEST_CASE("clicking the chosen tile again does nothing", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestChoosing::test_clicking_the_chosen_tile_again_does_nothing
    Choosing fixture;
    QStringList requested;
    QObject::connect(fixture.p.get(), &AgentPicker::choiceRequested,
                     [&](const QString& id) { requested << id; });
    QTest::mouseClick(fixture.p->tile(builtin()), Qt::LeftButton, {}, QPoint(20, 20));
    CHECK(requested.isEmpty());
}

TEST_CASE("get link opens the install page", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestChoosing::test_get_link_opens_the_install_page
    Choosing fixture;
    AgentTile* tile = fixture.p->tile(kOpenCode);
    CHECK_FALSE(tile->link()->isHidden());
    CHECK(tile->link()->toolTip() == QStringLiteral("npm install -g opencode-ai"));
    UrlCatcher catcher;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "open");
    tile->link()->click();
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    REQUIRE(catcher.opened.size() == 1);
    CHECK(catcher.opened.front().toString() == QStringLiteral("https://opencode.ai/docs/"));
    CHECK(fixture.p->tile(kClaudeCode)->link()->isHidden());
}

TEST_CASE("choosing an agent shows its models and sign in", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestChoosing::test_choosing_an_agent_shows_its_models_and_sign_in
    Choosing fixture;
    AgentPicker& p = *fixture.p;
    p.setSavedModels({{kClaudeCode, QStringLiteral("sonnet")}, {kCodex, QStringLiteral("gpt-x")}});
    p.setChoice(kClaudeCode);
    CHECK(p.tile(kClaudeCode)->isSelected());
    CHECK_FALSE(p.tile(builtin())->isSelected());
    CHECK_FALSE(p.modelCard()->isHidden());
    QComboBox* combo = p.modelCombo();
    CHECK(combo == static_cast<QComboBox*>(p.shortModelCombo()));
    CHECK(combo->itemText(0) == QStringLiteral("Claude Code default (opus)"));
    CHECK(combo->currentData().toString() == QStringLiteral("sonnet"));
    CHECK(p.usageLabel()->text().contains(QStringLiteral("Claude Team")));
    CHECK(p.notice()->isHidden());

    std::vector<std::pair<QString, QString>> chosen;
    QObject::connect(&p, &AgentPicker::modelChosen, [&](const QString& id, const QString& model) {
        chosen.emplace_back(id, model);
    });
    Q_EMIT combo->activated(combo->findData(QStringLiteral("haiku")));
    CHECK(chosen ==
          std::vector<std::pair<QString, QString>>{{kClaudeCode, QStringLiteral("haiku")}});
    Q_EMIT combo->activated(combo->findData(QStringLiteral("haiku")));
    CHECK(chosen ==
          std::vector<std::pair<QString, QString>>{{kClaudeCode, QStringLiteral("haiku")}});
}

TEST_CASE("a saved model the agent no longer lists is kept", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestChoosing::test_a_saved_model_the_agent_no_longer_lists_is_kept
    Choosing fixture;
    fixture.p->setSavedModels({{kCodex, QStringLiteral("gpt-retired")}});
    fixture.p->setChoice(kCodex);
    QComboBox* combo = fixture.p->modelCombo();
    CHECK(combo->currentData().toString() == QStringLiteral("gpt-retired"));
    CHECK(combo->itemText(0) == QStringLiteral("Codex default"));
}

TEST_CASE("models are listed once until look again clears them", "[widget]") {
    // start_scan(refresh=True) clears _models, so the next choice lists the agent's models again.
    Choosing fixture;
    auto lists = std::make_shared<std::atomic<int>>(0);
    fixture.scope.seam().list_agent_models = [lists](const InstalledAgent& agent) {
        ++*lists;
        return models(agent);
    };
    fixture.scope.seam().scan_installed_agents = [](bool) {
        return foundTwo();
    };
    AgentPicker& p = *fixture.p;
    p.setChoice(kClaudeCode);
    CHECK(*lists == 1);
    p.setChoice(builtin());
    p.setChoice(kClaudeCode);
    CHECK(*lists == 1);
    CHECK(p.modelCombo()->count() == 3);

    p.setChoice(builtin());
    p.rescan();
    REQUIRE(pumpUntil([&] { return !p.isScanning(); }));
    p.setChoice(kClaudeCode);
    CHECK(*lists == 2);
    CHECK(p.modelCombo()->count() == 3);
}

TEST_CASE("a model list that fails on the gui thread offers only the default", "[widget]") {
    // _show_model_card catches whatever list_agent_models raises; here it is not even a
    // std::exception, and it must not reach the event loop.
    Choosing fixture;
    fixture.scope.seam().list_agent_models = [](const InstalledAgent&) -> std::vector<AgentModel> {
        throw 42;
    };
    fixture.p->setChoice(kClaudeCode);
    QComboBox* combo = fixture.p->modelCombo();
    REQUIRE(combo->count() == 1);
    CHECK(combo->itemText(0) == QStringLiteral("Claude Code default"));
    CHECK(combo->currentData().toString().isEmpty());
    CHECK_FALSE(fixture.p->modelCard()->isHidden());
}

TEST_CASE("built in choice hides the model row", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestChoosing::test_built_in_choice_hides_the_model_row
    Choosing fixture;
    fixture.p->setChoice(QStringLiteral("direct"));
    CHECK(fixture.p->choice() == builtin());
    CHECK(fixture.p->tile(builtin())->isSelected());
    CHECK(fixture.p->modelCard()->isHidden());
}

TEST_CASE("a chosen agent that is signed out says why", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestChoosing::test_a_chosen_agent_that_is_signed_out_says_why
    SeamScope scope;
    const auto signedOut = agent("codex", "codex", "0.158.0", "", false);
    scope.cache(ScanResult{{"claude_code", kClaude}, {"codex", signedOut}});
    AgentPicker p;
    p.setChoice(kCodex);
    AgentTile* tile = p.tile(kCodex);
    CHECK(tile->isSelected());
    CHECK(tile->state().tone == "warning");
    CHECK(tile->pill()->text() == QStringLiteral("Sign in needed"));
    CHECK_FALSE(p.notice()->isHidden());
    CHECK(p.modelCard()->isHidden());
}

// ---- TestOpenCodeModels ----

TEST_CASE("models load off the ui thread and filter when long", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestOpenCodeModels::test_models_load_off_the_ui_thread_and_filter_when_long
    SeamScope scope;
    auto gate = std::make_shared<Gate>();
    const std::thread::id gui = std::this_thread::get_id();
    auto onGui = std::make_shared<std::vector<bool>>();
    auto onGuiMutex = std::make_shared<std::mutex>();
    std::vector<AgentModel> many{{"", "OpenCode default"}};
    for (int i = 0; i < picker::kFilterThreshold + 10; ++i) {
        many.push_back({"provider/model-" + std::to_string(i), "Model " + std::to_string(i)});
    }
    scope.seam().list_agent_models = [gate, gui, onGui, onGuiMutex, many](const InstalledAgent&) {
        {
            std::lock_guard lock(*onGuiMutex);
            onGui->push_back(std::this_thread::get_id() == gui);
        }
        gate->wait();
        return many;
    };
    scope.cache(ScanResult{
        {"claude_code", std::nullopt}, {"codex", std::nullopt}, {"opencode_cli", kOpenCodeAgent}});
    AgentPicker p;
    p.setSavedModels({{kOpenCode, QStringLiteral("provider/model-3")}});
    p.setChoice(kOpenCode);
    CHECK(p.modelStatus()->text() == QStringLiteral(u"Loading OpenCode's models…"));
    CHECK_FALSE(p.modelStatus()->isHidden());
    CHECK_FALSE(p.shortModelCombo()->isEnabled());
    gate->open();
    CHECK(pumpUntil([&] { return p.modelCombo() == static_cast<QComboBox*>(p.longModelCombo()); }));
    {
        std::lock_guard lock(*onGuiMutex);
        CHECK(*onGui == std::vector<bool>{false});
    }
    fv::SearchableComboBox* combo = p.longModelCombo();
    CHECK_FALSE(combo->isHidden());
    CHECK(p.shortModelCombo()->isHidden());
    CHECK(combo->count() == static_cast<int>(many.size()));
    CHECK(combo->currentData().toString() == QStringLiteral("provider/model-3"));
    CHECK(p.modelStatus()->isHidden());

    QStringList chosen;
    QObject::connect(&p, &AgentPicker::modelChosen,
                     [&](const QString&, const QString& model) { chosen << model; });
    Q_EMIT combo->activated(combo->findData(QStringLiteral("provider/model-7")));
    CHECK(chosen == QStringList{QStringLiteral("provider/model-7")});
    // A filter fragment left in the editor reverts to the choice.
    combo->lineEdit()->setText(QStringLiteral("mod"));
    Q_EMIT combo->lineEdit()->editingFinished();
    CHECK(combo->currentText() == QStringLiteral("Model 7"));
}

TEST_CASE("an empty model list says opencode uses its default", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestOpenCodeModels::test_an_empty_model_list_says_opencode_uses_its_default
    SeamScope scope;
    scope.cache(ScanResult{{"opencode_cli", kOpenCodeAgent}});
    scope.seam().list_agent_models = [](const InstalledAgent&) {
        return std::vector<AgentModel>{{"", "OpenCode default"}};
    };
    AgentPicker p;
    p.setChoice(kOpenCode);
    CHECK(
        pumpUntil([&] { return p.modelStatus()->text().contains(QStringLiteral("didn't list")); }));
    CHECK(p.modelCombo()->count() == 1);
}

// ---- TestLayout ----

TEST_CASE("tiles wrap two by two when narrow", "[test_agent_picker][widget]") {
    // from tests/test_agent_picker.py::TestLayout::test_tiles_wrap_two_by_two_when_narrow
    SeamScope scope;
    scope.cache(foundTwo());
    AgentPicker p;
    p.resize(1000, 400);
    p.show();
    pumpFor(std::chrono::milliseconds(50));
    CHECK(p.grid()->columns() == 4);
    const std::vector<AgentTile*> tiles{p.tile(kClaudeCode), p.tile(kCodex), p.tile(kOpenCode),
                                        p.tile(builtin())};
    for (AgentTile* tile : tiles) {
        CHECK(tile->y() == tiles[0]->y());
    }
    p.resize(480, 600);
    pumpFor(std::chrono::milliseconds(50));
    CHECK(p.grid()->columns() == 2);
    CHECK(tiles[0]->y() == tiles[1]->y());
    CHECK(tiles[1]->y() < tiles[2]->y());
    CHECK(tiles[2]->y() == tiles[3]->y());
    CHECK(p.grid()->height() >= tiles[3]->geometry().bottom() - 8);
    p.close();
}

TEST_CASE("a long problem grows its row instead of overlapping", "[test_agent_picker][widget]") {
    // from
    // tests/test_agent_picker.py::TestLayout::test_a_long_problem_grows_its_row_instead_of_overlapping
    SeamScope scope;
    const auto old = agent("opencode_cli", "opencode", "1.4.2", "", std::nullopt,
                           std::string(fv::identity().product) +
                               " needs OpenCode 2.0.0 or newer. Run `opencode upgrade`.");
    scope.cache(
        ScanResult{{"claude_code", kClaude}, {"codex", kCodexAgent}, {"opencode_cli", old}});
    AgentPicker p;
    // Just wide enough for one row: the text wraps more than it would at the label's preferred
    // width.
    p.resize(4 * fv::TileGrid::kMinCard + 3 * fv::TileGrid::kGap + 10, 400);
    p.show();
    pumpFor(std::chrono::milliseconds(100));
    CHECK(p.grid()->columns() == 4);
    AgentTile* tile = p.tile(kOpenCode);
    QLabel* detail = tile->detailLabel();
    CHECK(detail->heightForWidth(detail->width()) > detail->fontMetrics().height());
    CHECK(tile->nameLabel()->geometry().bottom() < detail->geometry().top());
    CHECK(detail->geometry().bottom() <= tile->cardRect().bottom());
    CHECK(detail->height() >= detail->heightForWidth(detail->width()));
    // Every tile in the row takes the tallest one's height.
    for (AgentTile* other : p.tiles()) {
        CHECK(other->height() == tile->height());
    }
    p.close();
}

// ---- animations at fixed times ----

TEST_CASE("an arrival fades the pill in and draws its check at fixed times", "[widget]") {
    SeamScope scope;
    AgentTile tile(kClaudeCode, QStringLiteral("Claude Code"), QStringLiteral("CC"),
                   QColor(QStringLiteral("#D97757")));
    tile.showState(picker::describe_agent("claude_code", kClaude), true);
    QVariantAnimation* arrival = tile.arrivalAnimation();
    CHECK(arrival->duration() == picker::kArrivalMs);
    CHECK(tile.arrival() == 0.0);
    CHECK(tile.pill()->appear() == 0.0);
    CHECK(tile.pill()->check() == 0.0);
    const auto expectAt = [&](int ms) {
        arrival->setCurrentTime(ms);
        const double t = static_cast<double>(ms) / picker::kArrivalMs;
        CHECK(tile.arrival() == Approx(t));
        CHECK(tile.pill()->appear() == Approx(fv::agent_paint::easeOut((t - 0.12) / 0.45)));
        CHECK(tile.pill()->check() == Approx(fv::agent_paint::easeInOut((t - 0.35) / 0.6)));
        CHECK(tile.mark()->warmth() == Approx(fv::agent_paint::easeOut(t / 0.75)));
    };
    expectAt(95);
    expectAt(190);
    expectAt(285);
    arrival->setCurrentTime(picker::kArrivalMs);
    CHECK(tile.arrival() == 1.0);
    CHECK(tile.pill()->appear() == 1.0);
    CHECK(tile.pill()->check() == 1.0);
    CHECK(tile.mark()->warmth() == 1.0);
}

TEST_CASE("a chosen tile lifts by 2 px over 220 ms and hover lifts a selectable one", "[widget]") {
    SeamScope scope;
    AgentTile tile(kCodex, QStringLiteral("Codex"), QStringLiteral("Cx"),
                   QColor(QStringLiteral("#10A37F")));
    tile.resize(200, 140);
    tile.showState(picker::describe_agent("codex", kCodexAgent));
    const double restTop = tile.cardRect().top();
    tile.setSelected(true);
    QVariantAnimation* lift = tile.liftAnimation();
    CHECK(lift->duration() == picker::kLiftMs);
    lift->setCurrentTime(110);
    CHECK(tile.lift() == Approx(0.5));
    CHECK(tile.cardRect().top() == Approx(restTop - 1.0));
    lift->setCurrentTime(picker::kLiftMs);
    CHECK(tile.cardRect().top() == Approx(restTop - AgentTile::kLiftPx));
    tile.setSelected(false, false);
    CHECK(tile.cardRect().top() == Approx(restTop));

    QEvent enter(QEvent::HoverEnter);
    QCoreApplication::sendEvent(&tile, &enter);
    QVariantAnimation* hover = tile.hoverAnimation();
    CHECK(hover->duration() == picker::kHoverMs);
    hover->setCurrentTime(70);
    CHECK(tile.hover() == Approx(0.5));
    hover->setCurrentTime(picker::kHoverMs);
    CHECK(tile.cardRect().top() == Approx(restTop - 1.0));

    // A missing agent's tile does not take the hover.
    AgentTile missing(kOpenCode, QStringLiteral("OpenCode"), QStringLiteral("OC"),
                      QColor(QStringLiteral("#6D5DD3")));
    missing.showState(picker::describe_agent("opencode_cli", std::nullopt));
    QCoreApplication::sendEvent(&missing, &enter);
    CHECK(missing.hover() == 0.0);
    CHECK(missing.hoverAnimation()->state() == QAbstractAnimation::Stopped);
}

TEST_CASE("the shimmer sweeps once per period with a per tile offset", "[widget]") {
    SeamScope scope;
    AgentTile first(kClaudeCode, QStringLiteral("Claude Code"), QStringLiteral("CC"),
                    QColor(QStringLiteral("#D97757")));
    CHECK(first.scanPhase() == 0.0);
    CHECK(first.scanBreath() == 0.0);
    first.beginScan(0);
    CHECK(first.shimmer() == 1.0);
    CHECK(first.detailLabel()->text() == QStringLiteral(u"Checking…"));
    // Starts 0.3 of a sweep in, so a scan shorter than one sweep still shows the band.
    // The phase only moves on from where it starts; up to 0.15 of a sweep (195 ms) is allowed
    // for a busy machine between beginScan and the read.
    const double firstPhase = first.scanPhase();
    CHECK(firstPhase >= Approx(0.3));
    CHECK(firstPhase < 0.45);
    AgentTile third(kOpenCode, QStringLiteral("OpenCode"), QStringLiteral("OC"),
                    QColor(QStringLiteral("#6D5DD3")));
    third.beginScan(2);
    // Two tiles to the left: it trails by 280 ms, 1690 - 280 = 1410 ms, 110 ms into a sweep.
    const double thirdPhase = third.scanPhase();
    CHECK(thirdPhase >= Approx(110.0 / 1300.0));
    CHECK(thirdPhase < 110.0 / 1300.0 + 0.15);
    first.endScan(true);
    QVariantAnimation* fade = first.shimmerAnimation();
    CHECK(fade->duration() == picker::kShimmerFadeMs);
    fade->setCurrentTime(80);
    CHECK(first.shimmer() == Approx(0.5));
    fade->setCurrentTime(picker::kShimmerFadeMs);
    CHECK(first.shimmer() == 0.0);
    CHECK(first.scanBreath() == 0.0);
}

// ---- the default seam and the lookup relay ----

TEST_CASE("the default seam scans through the set installed agents", "[widget]") {
    picker::resetScanSeam();
    auto fake = std::make_shared<agents::FakeInstalledAgents>(foundTwo());
    agents::set_installed_agents(fake);
    {
        AgentPicker p;
        CHECK_FALSE(p.agents().has_value()); // nothing cached before the first scan
        p.ensureScanned();
        CHECK(pumpUntil([&] { return !p.isScanning(); }));
        CHECK(p.agent(kClaudeCode) == std::optional<InstalledAgent>(kClaude));
        CHECK(p.agent(kOpenCode) == std::nullopt);
        CHECK(fake->lookups() == 3);
        // A second picker shows the instance's cached scan without scanning again.
        AgentPicker second;
        CHECK(second.agents().has_value());
        CHECK(fake->lookups() == 3);
    }
    CHECK(picker::waitForWorkers(std::chrono::seconds(5)));
    QCoreApplication::processEvents();
    agents::set_installed_agents(nullptr);
    picker::resetScanSeam();
}

TEST_CASE("with no installed agents set a scan reads as nothing found", "[widget]") {
    picker::resetScanSeam();
    agents::set_installed_agents(nullptr);
    AgentPicker p;
    p.ensureScanned();
    CHECK(pumpUntil([&] { return !p.isScanning(); }));
    CHECK(p.agents().has_value());
    CHECK(p.agent(kClaudeCode) == std::nullopt);
    CHECK(picker::waitForWorkers(std::chrono::seconds(5)));
}

TEST_CASE("the lookup relay brings lookups to the gui thread", "[widget]") {
    // ui_qt/utils/agent_lookup.py: a scan notifies every agent id from its own thread.
    auto fake = std::make_shared<agents::FakeInstalledAgents>(foundTwo());
    QStringList seen;
    std::vector<bool> onGui;
    const std::thread::id gui = std::this_thread::get_id();
    {
        fv::AgentLookupRelay relay(*fake);
        QObject::connect(&relay, &fv::AgentLookupRelay::lookedUp, [&](const QString& id) {
            seen << id;
            onGui.push_back(std::this_thread::get_id() == gui);
        });
        std::thread scanner([&fake] { fake->scan_agents(true); });
        scanner.join();
        CHECK(pumpUntil([&] { return seen.size() == 3; }));
        CHECK(seen == QStringList{kClaudeCode, kCodex, kOpenCode});
        CHECK(onGui == std::vector<bool>{true, true, true});
    }
    // Once the relay is gone a lookup reaches nobody.
    std::thread scanner([&fake] { fake->scan_agents(true); });
    scanner.join();
    QCoreApplication::processEvents();
    CHECK(seen.size() == 3);
}

TEST_CASE("the session lookup relay follows the set installed agents", "[widget]") {
    agents::set_installed_agents(nullptr);
    CHECK(fv::agentLookupRelay() == nullptr);
    auto fake = std::make_shared<agents::FakeInstalledAgents>();
    agents::set_installed_agents(fake);
    fv::AgentLookupRelay* relay = fv::agentLookupRelay();
    REQUIRE(relay != nullptr);
    CHECK(fv::agentLookupRelay() == relay);
    CHECK(relay->parent() == QCoreApplication::instance());
    agents::set_installed_agents(nullptr);
    CHECK(fv::agentLookupRelay() == nullptr);
}

#include "test_agent_picker.moc"
