#include "ui_widgets/agent_picker.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/number.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/detail/thread_name.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace picker = agent_picker;

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.agent_picker"};

// MeetingAgentCore.OPENCODE: the packaged OpenCode SDK, which the built-in tile then names.
constexpr std::string_view kOpenCodeSdkCore = "opencode";

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

int pyRound(double value) {
    return static_cast<int>(py::round_half_even(value));
}

QString builtinId() {
    return qs(legacy_builtin_agent_id);
}

std::shared_ptr<agents::InstalledAgents> setAgents() {
    auto agents = agents::installed_agents_if_set();
    if (!agents) {
        throw std::logic_error("No installed-agents instance is set.");
    }
    return agents;
}

picker::ScanSeam defaultSeam() {
    return {
        [](bool refresh) { return setAgents()->scan_agents(refresh); },
        []() -> std::optional<agents::ScanResult> {
            const auto agents = agents::installed_agents_if_set();
            return agents ? agents->cached_scan() : std::nullopt;
        },
        [](const agents::InstalledAgent& agent) { return setAgents()->list_models(agent); },
    };
}

// ---- the session relay ----
//
// Brings scan and model-list results to the GUI thread for whichever pickers exist then. A
// worker never holds or posts to a picker: holding one let a Python worker delete the widget off
// the GUI thread, and emitting on one raced Settings closing. Workers post to an executor whose
// context is the application, which drops anything posted once the application is gone, and the
// GUI thread hands the result to the pickers still registered.

struct Workers {
    std::mutex mutex;
    std::condition_variable idle;
    int running = 0;
};

Workers& workers() {
    // Never destroyed: a detached worker may still finish after static destruction began.
    static auto* instance = new Workers();
    return *instance;
}

std::vector<QPointer<AgentPicker>>& livePickers() {
    static std::vector<QPointer<AgentPicker>> pickers;
    return pickers;
}

std::shared_ptr<Executor> relayExecutor() {
    static std::shared_ptr<Executor> executor;
    static QPointer<QCoreApplication> madeFor;
    if (!executor || madeFor.isNull() || madeFor.data() != QCoreApplication::instance()) {
        madeFor = QCoreApplication::instance();
        executor = std::make_shared<qt::QtUiExecutor>(QCoreApplication::instance());
    }
    return executor;
}

void registerPicker(AgentPicker* picker) {
    auto& pickers = livePickers();
    std::erase_if(pickers, [](const QPointer<AgentPicker>& p) { return p.isNull(); });
    pickers.emplace_back(picker);
}

template <class Fn> void forEachPicker(Fn&& fn) {
    // A copy: a picker's handler may create or delete pickers.
    const std::vector<QPointer<AgentPicker>> pickers = livePickers();
    for (const QPointer<AgentPicker>& picker : pickers) {
        if (!picker.isNull()) {
            fn(picker.data());
        }
    }
}

// Request tokens, unique across pickers because they share the relay; the first is 1.
std::uint64_t nextToken() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1);
}

// Runs work on a detached, named thread counted by waitForWorkers.
void startWorker(std::string name, std::function<void()> work) {
    {
        std::lock_guard lock(workers().mutex);
        ++workers().running;
    }
    std::thread([name = std::move(name), work = std::move(work)] {
        ui_widgets_detail::setCurrentThreadName(name);
        try {
            work();
        } catch (...) {
            // work catches what its blocking call throws; nothing else may escape a thread.
        }
        Workers& state = workers();
        std::lock_guard lock(state.mutex);
        --state.running;
        state.idle.notify_all();
    }).detach();
}

} // namespace

namespace agent_picker {

ScanSeam& scanSeam() {
    static ScanSeam seam = defaultSeam();
    return seam;
}

void resetScanSeam() {
    scanSeam() = defaultSeam();
}

int runningWorkers() {
    std::lock_guard lock(workers().mutex);
    return workers().running;
}

bool waitForWorkers(std::chrono::milliseconds timeout) {
    std::unique_lock lock(workers().mutex);
    return workers().idle.wait_for(lock, timeout, [] { return workers().running == 0; });
}

} // namespace agent_picker

// ---- TileGrid ----

TileGrid::TileGrid(const std::vector<AgentTile*>& tiles, QWidget* parent)
    : QWidget(parent), tiles_(tiles) {
    setObjectName(QStringLiteral("agentTileGrid"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    for (AgentTile* tile : tiles_) {
        tile->setParent(this);
    }
}

int TileGrid::columns(std::optional<int> width) const {
    const int at = width.value_or(this->width());
    const int card = pyRound(kMinCard * ui::currentUiFontScale());
    if (at >= 4 * card + 3 * kGap) {
        return 4;
    }
    if (at >= 2 * card + kGap) {
        return 2;
    }
    return 1;
}

TileGrid::Plan TileGrid::plan(int width) const {
    Plan result;
    result.columns = columns(width);
    result.cell = static_cast<double>(width - kGap * (result.columns - 1)) / result.columns;
    const int extra = AgentTile::kBleedTop + AgentTile::kBleedBottom;
    for (std::size_t start = 0; start < tiles_.size();
         start += static_cast<std::size_t>(result.columns)) {
        Row row;
        const std::size_t end =
            std::min(tiles_.size(), start + static_cast<std::size_t>(result.columns));
        row.tiles.assign(tiles_.begin() + static_cast<std::ptrdiff_t>(start),
                         tiles_.begin() + static_cast<std::ptrdiff_t>(end));
        int tallest = 0;
        bool first = true;
        for (AgentTile* tile : row.tiles) {
            const int height =
                tile->hasHeightForWidth()
                    ? tile->heightForWidth(pyRound(result.cell) + 2 * AgentTile::kBleedX)
                    : tile->sizeHint().height();
            tallest = first ? height : std::max(tallest, height);
            first = false;
        }
        row.height = tallest - extra;
        result.rows.push_back(std::move(row));
    }
    return result;
}

void TileGrid::layoutTiles() {
    const int width = this->width();
    if (width <= 0) {
        return;
    }
    const Plan layout = plan(width);
    double y = AgentTile::kBleedTop;
    for (const Row& row : layout.rows) {
        for (std::size_t index = 0; index < row.tiles.size(); ++index) {
            const double x = static_cast<double>(index) * (layout.cell + kGap);
            row.tiles[index]->setGeometry(
                pyRound(x) - AgentTile::kBleedX, pyRound(y) - AgentTile::kBleedTop,
                pyRound(x + layout.cell) - pyRound(x) + 2 * AgentTile::kBleedX,
                row.height + AgentTile::kBleedTop + AgentTile::kBleedBottom);
        }
        y += row.height + kGap;
    }
    const int total = pyRound(y - kGap) + AgentTile::kBleedBottom;
    if (total != height_) {
        height_ = total;
        updateGeometry();
    }
}

QSize TileGrid::sizeHint() const {
    const int width = this->width() > 0 ? this->width() : 720;
    const Plan layout = plan(width);
    int height = 0;
    for (const Row& row : layout.rows) {
        height += row.height;
    }
    height += kGap * (static_cast<int>(layout.rows.size()) - 1);
    height += AgentTile::kBleedTop + AgentTile::kBleedBottom;
    return {2 * pyRound(kMinCard * ui::currentUiFontScale()) + kGap, height};
}

QSize TileGrid::minimumSizeHint() const {
    return {0, sizeHint().height()};
}

void TileGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutTiles();
    if (event->oldSize().width() != event->size().width()) {
        updateGeometry();
    }
}

void TileGrid::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    layoutTiles();
}

bool TileGrid::event(QEvent* event) {
    // A tile's text changed height; its parent has no layout to tell.
    if (event->type() == QEvent::LayoutRequest) {
        layoutTiles();
    }
    return QWidget::event(event);
}

// ---- AgentPicker ----

AgentPicker::AgentPicker(const QString& purpose, bool framedModels, QWidget* parent)
    : QWidget(parent), purpose_(purpose), choice_(builtinId()) {
    setObjectName(QStringLiteral("agentPicker"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);
    auto* heading = new QLabel(QStringLiteral("Who runs %1").arg(purpose), this);
    heading->setObjectName(QStringLiteral("settingsTileGroupTitle"));
    header->addWidget(heading, 0, Qt::AlignBottom);
    header->addStretch(1);
    lookAgainButton_ = neutralButton(new Button(QStringLiteral("Look again")));
    lookAgainButton_->setObjectName(QStringLiteral("agentLookAgainButton"));
    lookAgainButton_->setIcon(ui::designIcon(QStringLiteral("refresh-blue.svg")));
    lookAgainButton_->setIconSize(QSize(16, 16));
    lookAgainButton_->setToolTip(
        QStringLiteral("Check this computer again, for example after installing an agent"));
    connect(lookAgainButton_, &QPushButton::clicked, this, &AgentPicker::rescan);
    header->addWidget(lookAgainButton_);
    layout->addLayout(header);

    countLabel_ = new WrappedLabel(
        QStringLiteral("Coding agents you already use can run %1 with their own sign-in.")
            .arg(purpose),
        this);
    countLabel_->setObjectName(QStringLiteral("infoLabel"));
    layout->addWidget(countLabel_);

    for (const agents::AgentSpec& spec : agents::agent_specs()) {
        tiles_.push_back(new AgentTile(qs(spec.id), qs(spec.name), qs(spec.monogram),
                                       QColor(qs(spec.accent)), QPixmap(), qs(spec.install_url),
                                       qs(spec.install_hint)));
    }
    auto* builtin = new AgentTile(builtinId(), QStringLiteral("Pi"), QStringLiteral(u"π"),
                                  QColor(QStringLiteral("#0a84ff")));
    builtin->showState(picker::builtin_state());
    if (purpose == qs(picker::kCleanupPurpose)) {
        builtin->setIdentity(QStringLiteral("API / local model"), QStringLiteral("AI"));
        builtin->showState(picker::cleanup_builtin_state());
    }
    tiles_.push_back(builtin);
    for (AgentTile* tile : tiles_) {
        tile->setPurpose(purpose);
        connect(tile, &AgentTile::clicked, this, &AgentPicker::onTileClicked);
    }
    grid_ = new TileGrid(tiles_, this);
    layout->addWidget(grid_);

    notice_ = new QFrame(this);
    notice_->setObjectName(QStringLiteral("settingsTile"));
    ui::setStyleProperty(notice_, ui::prop::kind, QStringLiteral("notice"));
    auto* noticeRow = new QHBoxLayout(notice_);
    noticeRow->setContentsMargins(14, 10, 14, 10);
    noticeRow->setSpacing(10);
    auto* noticeIcon = new QLabel(notice_);
    noticeIcon->setObjectName(QStringLiteral("settingsTileIcon"));
    noticeIcon->setFixedSize(18, 18);
    noticeIcon->setPixmap(ui::designIcon(QStringLiteral("info-warning.svg")).pixmap(16, 16));
    noticeRow->addWidget(noticeIcon, 0, Qt::AlignTop);
    noticeLabel_ = new WrappedLabel(QString(), notice_);
    noticeLabel_->setObjectName(QStringLiteral("settingsTileDescription"));
    noticeRow->addWidget(noticeLabel_, 1);
    notice_->hide();
    layout->addWidget(notice_);

    QVBoxLayout* card = nullptr;
    if (framedModels) {
        auto* frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("settingsTile"));
        ui::setStyleProperty(frame, ui::prop::kind, QStringLiteral("field"));
        modelCard_ = frame;
        card = new QVBoxLayout(modelCard_);
        card->setContentsMargins(16, 14, 16, 14);
    } else {
        // The host puts the model row in a card of its own, beside the settings that apply to
        // every choice.
        modelCard_ = new QWidget(this);
        modelCard_->setObjectName(QStringLiteral("agentModelSection"));
        card = new QVBoxLayout(modelCard_);
        card->setContentsMargins(0, 0, 0, 0);
    }
    card->setSpacing(8);
    modelField_ = new QWidget(modelCard_);
    modelField_->setObjectName(QStringLiteral("modelManagerFieldGroup"));
    auto* field = new QVBoxLayout(modelField_);
    field->setContentsMargins(0, 0, 0, 0);
    field->setSpacing(5);
    modelFieldLabel_ = new QLabel(QStringLiteral("Model"), modelField_);
    modelFieldLabel_->setObjectName(QStringLiteral("textModelFieldLabel"));
    field->addWidget(modelFieldLabel_);
    shortModelCombo_ = new ElidingComboBox(modelField_);
    shortModelCombo_->setObjectName(QStringLiteral("agentModelCombo"));
    shortModelCombo_->setMinimumHeight(40);
    connect(shortModelCombo_, &QComboBox::activated, this,
            [this](int index) { commitModel(shortModelCombo_, index); });
    field->addWidget(shortModelCombo_);
    longModelCombo_ = new SearchableComboBox(modelField_);
    longModelCombo_->setObjectName(QStringLiteral("agentModelSearchCombo"));
    longModelCombo_->setMinimumHeight(40);
    connect(longModelCombo_, &QComboBox::activated, this,
            [this](int index) { commitModel(longModelCombo_, index); });
    connect(longModelCombo_->lineEdit(), &QLineEdit::editingFinished, this,
            &AgentPicker::restoreLongModelText);
    longModelCombo_->lineEdit()->setPlaceholderText(QStringLiteral("Type to filter models"));
    longModelCombo_->hide();
    field->addWidget(longModelCombo_);
    card->addWidget(modelField_);
    modelStatus_ = new WrappedLabel(QString(), modelCard_);
    modelStatus_->setObjectName(QStringLiteral("infoLabel"));
    modelStatus_->hide();
    card->addWidget(modelStatus_);
    usageLabel_ = new WrappedLabel(QString(), modelCard_);
    usageLabel_->setObjectName(QStringLiteral("infoLabel"));
    card->addWidget(usageLabel_);
    modelCard_->hide();
    if (framedModels) {
        layout->addWidget(modelCard_);
    }

    tick_ = new QTimer(this);
    tick_->setInterval(picker::kTickMs);
    connect(tick_, &QTimer::timeout, this, &AgentPicker::onTick);
    registerPicker(this);

    const std::optional<agents::ScanResult> cached = picker::scanSeam().cached_agents();
    if (cached) {
        applyResults(*cached, false);
    }
    syncSelection();
}

AgentTile* AgentPicker::tile(const QString& id) const {
    const auto it = std::find_if(tiles_.begin(), tiles_.end(),
                                 [&](const AgentTile* tile) { return tile->tileId() == id; });
    return it == tiles_.end() ? nullptr : *it;
}

QComboBox* AgentPicker::modelCombo() const {
    if (longList_) {
        return longModelCombo_;
    }
    return shortModelCombo_;
}

std::optional<agents::InstalledAgent> AgentPicker::agent(const QString& id) const {
    if (!agents_) {
        return std::nullopt;
    }
    const auto it = agents_->find(id.toStdString());
    return it == agents_->end() ? std::nullopt : it->second;
}

// ---- scanning ----

void AgentPicker::ensureScanned() {
    if (agents_ || scanning_) {
        return;
    }
    const std::optional<agents::ScanResult> cached = picker::scanSeam().cached_agents();
    if (cached) {
        applyResults(*cached, false);
        return;
    }
    startScan(false);
}

void AgentPicker::rescan() {
    startScan(true);
}

void AgentPicker::startScan(bool refresh) {
    const std::uint64_t generation = nextToken();
    generation_ = generation;
    scanning_ = true;
    lookAgainButton_->setEnabled(false);
    countLabel_->setText(QStringLiteral(u"Looking for coding agents on this computer…"));
    const std::vector<std::string_view> order = agents::agent_order();
    for (std::size_t index = 0; index < order.size(); ++index) {
        tile(qs(order[index]))->beginScan(static_cast<int>(index));
    }
    if (refresh) {
        models_.clear();
    }
    tick_->start();
    // Bound here: the worker must not reach back into the picker.
    auto scan = picker::scanSeam().scan_installed_agents;
    std::shared_ptr<Executor> relay = relayExecutor();
    startWorker("settings-agent-scan", [scan = std::move(scan), relay, refresh, generation] {
        agents::ScanResult result;
        try {
            result = scan(refresh);
        } catch (...) {
            kLog.warning("Scanning for installed agents failed");
            result.clear();
        }
        relay->post([generation, result = std::move(result)] {
            forEachPicker([&](AgentPicker* picker) { picker->onScanDone(generation, result); });
        });
    });
}

void AgentPicker::onTick() {
    for (const std::string_view agentId : agents::agent_order()) {
        tile(qs(agentId))->advanceScan();
    }
}

void AgentPicker::onScanDone(std::uint64_t generation, const agents::ScanResult& result) {
    if (generation != generation_ || !scanning_) {
        return;
    }
    scanning_ = false;
    tick_->stop();
    applyResults(result, true);
}

void AgentPicker::applyResults(const agents::ScanResult& result, bool animate) {
    agents::ScanResult known;
    for (const std::string_view agentId : agents::agent_order()) {
        const auto it = result.find(std::string(agentId));
        known[std::string(agentId)] = it == result.end() ? std::nullopt : it->second;
    }
    agents_ = std::move(known);
    lookAgainButton_->setEnabled(true);
    const std::vector<std::string_view> order = agents::agent_order();
    for (std::size_t index = 0; index < order.size(); ++index) {
        AgentTile* agentTile = tile(qs(order[index]));
        agentTile->endScan(animate);
        agentTile->showState(
            picker::describe_agent(order[index], agents_->at(std::string(order[index]))), animate,
            static_cast<int>(index) * picker::kStaggerMs);
    }
    countLabel_->setText(
        QString::fromStdString(picker::purpose_count_text(*agents_, purpose_.toStdString())));
    syncSelection();
    Q_EMIT resultsChanged();
}

// ---- choice ----

void AgentPicker::setChoice(const QString& core) {
    const std::string value = core.toStdString();
    const std::vector<std::string_view> order = agents::agent_order();
    const bool isAgent = std::find(order.begin(), order.end(), value) != order.end();
    choice_ = isAgent ? core : builtinId();
    if (choice_ == builtinId() && purpose_ != qs(picker::kCleanupPurpose)) {
        if (value == kOpenCodeSdkCore) {
            tile(builtinId())->setIdentity(QStringLiteral("OpenCode SDK"), QStringLiteral("OC"));
        } else {
            tile(builtinId())->setIdentity(QStringLiteral("Pi"), QStringLiteral(u"π"));
        }
    }
    syncSelection();
}

void AgentPicker::setSavedModels(const std::map<QString, QString>& models) {
    savedModels_ = models;
    if (!shownModelsFor_.isEmpty()) {
        fillModels(shownModelsFor_);
    }
}

void AgentPicker::onTileClicked(const QString& tileId) {
    if (tileId != choice_) {
        Q_EMIT choiceRequested(tileId);
    }
}

void AgentPicker::syncSelection() {
    // A hidden page jumps to the choice; only a visible change lifts.
    const bool animate = isVisible();
    for (AgentTile* agentTile : tiles_) {
        agentTile->setSelected(agentTile->tileId() == choice_, animate);
    }
    if (choice_ == builtinId() || !agents_) {
        // Built in, or not scanned yet (the tile shows the scan): nothing to say below.
        notice_->hide();
        showModelCard(std::nullopt);
        return;
    }
    const std::optional<agents::InstalledAgent> chosen = agent(choice_);
    const QString message = QString::fromStdString(
        picker::purpose_blocked_notice(choice_.toStdString(), chosen, purpose_.toStdString()));
    noticeLabel_->setText(message);
    notice_->setVisible(!message.isEmpty());
    showModelCard(chosen && chosen->usable() ? chosen : std::nullopt);
}

// ---- models ----

void AgentPicker::showModelCard(const std::optional<agents::InstalledAgent>& agent) {
    // The field hides with the card so Settings search skips it too.
    modelField_->setVisible(agent.has_value());
    if (!agent) {
        shownModelsFor_.clear();
        modelCard_->hide();
        return;
    }
    const QString agentId = QString::fromStdString(agent->id);
    shownModelsFor_ = agentId;
    usageLabel_->setText(
        QString::fromStdString(picker::purpose_usage_caption(*agent, purpose_.toStdString())));
    modelCard_->show();
    if (models_.contains(agentId)) {
        fillModels(agentId);
        return;
    }
    if (agent->spec().transport == "acp") {
        loadModelsInBackground(*agent);
        return;
    }
    std::vector<agents::AgentModel> models;
    try {
        models = picker::scanSeam().list_agent_models(*agent);
    } catch (...) {
        // On the GUI thread, inside a slot: nothing may escape to the event loop.
        kLog.warning("Could not list {} models", agent->id);
        models.clear();
    }
    models_[agentId] = std::move(models);
    fillModels(agentId);
}

void AgentPicker::loadModelsInBackground(const agents::InstalledAgent& agent) {
    const QString name = qs(agent.spec().name);
    const QString agentId = QString::fromStdString(agent.id);
    setComboItems({agents::AgentModel{"", (name + QStringLiteral(" default")).toStdString()}},
                  QString());
    shortModelCombo_->setEnabled(false);
    modelStatus_->setText(QStringLiteral(u"Loading %1's models…").arg(name));
    modelStatus_->show();
    if (modelsLoading_.contains(agentId)) {
        return;
    }
    const std::uint64_t generation = nextToken();
    modelsLoading_[agentId] = generation;
    auto listModels = picker::scanSeam().list_agent_models;
    std::shared_ptr<Executor> relay = relayExecutor();
    startWorker("settings-" + agent.id + "-models",
                [listModels = std::move(listModels), relay, agent, agentId, generation] {
                    std::vector<agents::AgentModel> models;
                    try {
                        models = listModels(agent);
                    } catch (...) {
                        kLog.warning("Could not list {} models", agent.id);
                        models.clear();
                    }
                    relay->post([agentId, generation, models = std::move(models)] {
                        forEachPicker([&](AgentPicker* picker) {
                            picker->onModelsDone(agentId, generation, models);
                        });
                    });
                });
}

void AgentPicker::onModelsDone(const QString& agentId, std::uint64_t generation,
                               const std::vector<agents::AgentModel>& models) {
    const auto it = modelsLoading_.find(agentId);
    if (it == modelsLoading_.end() || it->second != generation) {
        return;
    }
    modelsLoading_.erase(it);
    models_[agentId] = models;
    if (shownModelsFor_ == agentId) {
        fillModels(agentId);
    }
}

void AgentPicker::fillModels(const QString& agentId) {
    const agents::AgentSpec& spec = agents::agent_spec(agentId.toStdString());
    const QString name = qs(spec.name);
    std::vector<agents::AgentModel> models;
    if (const auto it = models_.find(agentId); it != models_.end()) {
        models = it->second;
    }
    if (models.empty() || !models.front().value.empty()) {
        models.insert(models.begin(),
                      agents::AgentModel{"", (name + QStringLiteral(" default")).toStdString()});
    }
    const auto saved = savedModels_.find(agentId);
    const QString savedModel = saved == savedModels_.end() ? QString() : saved->second;
    const std::string savedValue = savedModel.toStdString();
    if (!savedModel.isEmpty() &&
        std::none_of(models.begin(), models.end(),
                     [&](const agents::AgentModel& model) { return model.value == savedValue; })) {
        models.push_back(agents::AgentModel{savedValue, savedValue});
    }
    setComboItems(models, savedModel);
    shortModelCombo_->setEnabled(true);
    if (spec.transport == "acp" && models.size() <= 1) {
        modelStatus_->setText(
            QStringLiteral("%1 didn't list any models, so it uses its own default.").arg(name));
        modelStatus_->show();
    } else {
        modelStatus_->hide();
    }
}

void AgentPicker::setComboItems(const std::vector<agents::AgentModel>& models,
                                const QString& current) {
    const bool longList = models.size() > static_cast<std::size_t>(picker::kFilterThreshold);
    longList_ = longList;
    QComboBox* combo = longList ? static_cast<QComboBox*>(longModelCombo_)
                                : static_cast<QComboBox*>(shortModelCombo_);
    shortModelCombo_->setVisible(!longList);
    longModelCombo_->setVisible(longList);
    const bool blocked = combo->blockSignals(true);
    combo->clear();
    for (const agents::AgentModel& model : models) {
        combo->addItem(QString::fromStdString(model.label), QString::fromStdString(model.value));
    }
    const int index = std::max(0, combo->findData(current));
    combo->setCurrentIndex(index);
    combo->blockSignals(blocked);
    combo->setToolTip(combo->itemText(index));
}

void AgentPicker::commitModel(QComboBox* combo, int index) {
    const QString agentId = shownModelsFor_;
    if (agentId.isEmpty() || index < 0) {
        return;
    }
    const bool blocked = combo->blockSignals(true);
    combo->setCurrentIndex(index);
    combo->blockSignals(blocked);
    combo->setToolTip(combo->itemText(index));
    const QString value = combo->itemData(index).toString();
    const auto saved = savedModels_.find(agentId);
    if (value == (saved == savedModels_.end() ? QString() : saved->second)) {
        return;
    }
    savedModels_[agentId] = value;
    Q_EMIT modelChosen(agentId, value);
}

void AgentPicker::restoreLongModelText() {
    SearchableComboBox* combo = longModelCombo_;
    const auto saved = savedModels_.find(shownModelsFor_);
    const int index =
        std::max(0, combo->findData(saved == savedModels_.end() ? QString() : saved->second));
    if (combo->currentText() != combo->itemText(index)) {
        const bool blocked = combo->blockSignals(true);
        combo->setCurrentIndex(index);
        combo->setEditText(combo->itemText(index));
        combo->blockSignals(blocked);
    }
}

} // namespace fv
