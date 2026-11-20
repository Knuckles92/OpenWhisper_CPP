#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <QSize>
#include <QString>
#include <QWidget>

#include "agents/installed_agents.h"
#include "ui_widgets/agent_tile.h"

class QComboBox;
class QFrame;
class QLabel;
class QTimer;

namespace fv {

class Button;
class ElidingComboBox;
class SearchableComboBox;
class WrappedLabel;

namespace agent_picker {

// The blocking work behind the picker (agent_picker.py scan_installed_agents, cached_agents,
// list_agent_models). The defaults go to the set agents::InstalledAgents instance: with none set,
// a scan and a model list throw std::logic_error (a scan then reads as nothing found) and there
// is no cached scan. Tests replace the members; a worker keeps the function it started with.
struct ScanSeam {
    // Scans this computer for agents. Blocking; the picker calls it off the GUI thread.
    std::function<agents::ScanResult(bool refresh)> scan_installed_agents;
    // The last scan, or nullopt before the first. Never blocks.
    std::function<std::optional<agents::ScanResult>()> cached_agents;
    // The agent's models, its own default first. Blocking for OpenCode.
    std::function<std::vector<agents::AgentModel>(const agents::InstalledAgent&)> list_agent_models;
};

// The seam in use. Affinity: GUI thread.
ScanSeam& scanSeam();
// Puts the defaults back.
void resetScanSeam();

// Picker workers (scans and model lists) still running, across every picker.
// Affinity: thread-safe.
int runningWorkers();
// Waits until no picker worker runs; false when timeout passed first.
bool waitForWorkers(std::chrono::milliseconds timeout);

} // namespace agent_picker

// Four tiles in a row, or two by two when the page is narrow, or one column (object name
// "agentTileGrid"). Placed by hand so each tile's shadow margin can bleed past the grid's edges:
// the cards line up with the other cards on the page. Every tile in a row takes the tallest
// one's height.
// Affinity: GUI thread.
class TileGrid : public QWidget {
    Q_OBJECT
public:
    static constexpr int kGap = 12;
    // Narrowest card, at 100 % font scale.
    static constexpr int kMinCard = 160;

    explicit TileGrid(const std::vector<AgentTile*>& tiles, QWidget* parent = nullptr);

    // Columns at width (default: the grid's own).
    int columns(std::optional<int> width = std::nullopt) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Row {
        std::vector<AgentTile*> tiles;
        int height;
    };
    struct Plan {
        int columns;
        double cell;
        std::vector<Row> rows;
    };
    Plan plan(int width) const;
    void layoutTiles();

    std::vector<AgentTile*> tiles_;
    int height_ = 0;
};

// The "Who runs AI insights" group (object name "agentPicker"): one tile per coding agent the
// app can drive (Claude Code, Codex, OpenCode) and the built-in one, a rescan, a notice when the
// chosen agent cannot run, and the model row of the chosen agent. The picker scans and reports;
// it saves nothing. The host saves a choice when choiceRequested or modelChosen fires and
// answers with setChoice and setSavedModels.
//
// A cached scan shows at once without replaying the arrival; a fresh one shimmers the tiles until
// it returns and then lights them up one after another. Scans and OpenCode's model list run on
// worker threads that hold no picker: results come back through a session relay with a token,
// and a stale token is ignored. With framedModels false, modelCard() is not laid out here: the
// host adds it to a card of its own.
// Affinity: GUI thread.
class AgentPicker : public QWidget {
    Q_OBJECT
public:
    explicit AgentPicker(const QString& purpose = QStringLiteral("AI insights"),
                         bool framedModels = true, QWidget* parent = nullptr);

    const QString& purpose() const { return purpose_; }

    // Tiles in AGENT_ORDER, then the built-in one.
    const std::vector<AgentTile*>& tiles() const { return tiles_; }
    // The tile of an agent id or of legacy_builtin_agent_id; null for any other id.
    AgentTile* tile(const QString& id) const;
    TileGrid* grid() const { return grid_; }
    Button* lookAgainButton() const { return lookAgainButton_; }
    WrappedLabel* countLabel() const { return countLabel_; }
    QFrame* notice() const { return notice_; }
    WrappedLabel* noticeLabel() const { return noticeLabel_; }
    // The framed card, or the bare section the host places.
    QWidget* modelCard() const { return modelCard_; }
    QWidget* modelField() const { return modelField_; }
    ElidingComboBox* shortModelCombo() const { return shortModelCombo_; }
    SearchableComboBox* longModelCombo() const { return longModelCombo_; }
    WrappedLabel* modelStatus() const { return modelStatus_; }
    WrappedLabel* usageLabel() const { return usageLabel_; }
    // The combo now showing the chosen agent's models.
    QComboBox* modelCombo() const;

    bool isScanning() const { return scanning_; }
    // The last result the tiles show, or nullopt before one arrives.
    std::optional<agents::ScanResult> agents() const { return agents_; }
    std::optional<agents::InstalledAgent> agent(const QString& id) const;

    // Shows what is on this computer: the cached scan, or a new one.
    void ensureScanned();
    // Looks again, for example after the user installs an agent.
    void rescan();
    void startScan(bool refresh);
    // The token of the newest scan this picker started.
    std::uint64_t generation() const { return generation_; }
    // A scan's result, as the relay delivers it; a stale generation is ignored.
    void onScanDone(std::uint64_t generation, const agents::ScanResult& result);

    // The chosen tile: an agent id, or legacy_builtin_agent_id.
    const QString& choice() const { return choice_; }
    // Shows core (a MeetingAgentCore value) as the chosen tile; anything but an agent id is the
    // built-in tile.
    void setChoice(const QString& core);
    void setSavedModels(const std::map<QString, QString>& models);

Q_SIGNALS:
    // A tile was chosen: an agent id, or legacy_builtin_agent_id.
    void choiceRequested(const QString& id);
    // (agent id, model) picked in the model row; "" is the agent's default.
    void modelChosen(const QString& agent, const QString& model);
    // A scan result was applied (fresh or cached).
    void resultsChanged();

private:
    void onTick();
    void applyResults(const agents::ScanResult& result, bool animate);
    void onTileClicked(const QString& tileId);
    void syncSelection();
    void showModelCard(const std::optional<agents::InstalledAgent>& agent);
    void loadModelsInBackground(const agents::InstalledAgent& agent);
    void onModelsDone(const QString& agentId, std::uint64_t generation,
                      const std::vector<agents::AgentModel>& models);
    void fillModels(const QString& agentId);
    void setComboItems(const std::vector<agents::AgentModel>& models, const QString& current);
    void commitModel(QComboBox* combo, int index);
    // A filter fragment is not a choice: shows the chosen model again.
    void restoreLongModelText();

    QString purpose_;
    std::optional<agents::ScanResult> agents_;
    bool scanning_ = false;
    std::uint64_t generation_ = 0;
    QString choice_;
    std::map<QString, QString> savedModels_;
    std::map<QString, std::vector<agents::AgentModel>> models_;
    std::map<QString, std::uint64_t> modelsLoading_;
    QString shownModelsFor_;
    bool longList_ = false;

    std::vector<AgentTile*> tiles_;
    Button* lookAgainButton_;
    WrappedLabel* countLabel_;
    TileGrid* grid_;
    QFrame* notice_;
    WrappedLabel* noticeLabel_;
    QWidget* modelCard_;
    QWidget* modelField_;
    QLabel* modelFieldLabel_;
    ElidingComboBox* shortModelCombo_;
    SearchableComboBox* longModelCombo_;
    WrappedLabel* modelStatus_;
    WrappedLabel* usageLabel_;
    QTimer* tick_;
};

} // namespace fv
