#include "ui_settings/fast_judgments_page.h"

#include <algorithm>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

#include <QPointer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/executor.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/detail/mailbox.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_window.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_fast_judgments"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

QIcon icon(const char* file) {
    return ui::designIcon(QString::fromLatin1(file));
}

// The titles of the Judge bar's choices (_JUDGE_TITLES) and of the text-model bar's
// (_JUDGE_TEXT_MODEL_TITLES), in kRouteIds' and kTextModelIds' order.
constexpr const char* kRouteTitles[] = {"TypeSafe", "OpenRouter", "Your text model"};
constexpr const char* kTextModelTitles[] = {"AI insights model", "AI cleanup model"};

template <std::size_t N> int indexOf(const std::string_view (&ids)[N], std::string_view id) {
    for (std::size_t i = 0; i < N; ++i) {
        if (ids[i] == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// settings.get(key, False) is True: only a stored JSON true counts.
bool storedTrue(const Json& settings, std::string_view key) {
    if (!settings.is_object()) {
        return false;
    }
    const auto found = settings.find(key);
    return found != settings.end() && found->is_boolean() && found->get<bool>();
}

SettingsPage& pageOf(QWidget* parent) {
    auto* page = qobject_cast<SettingsPage*>(parent);
    if (page == nullptr || page->dialog() == nullptr) {
        throw std::logic_error("The Fast judgments page is built inside the Settings window.");
    }
    return *page;
}

} // namespace

// --- Words ---

namespace fast_judgments {

std::vector<llm::JudgeRoute> routes(const Json& settings) {
    return {llm::typesafe_route(), llm::openrouter_route(), llm::text_model_route(settings)};
}

std::optional<llm::JudgeRoute> other_route(const llm::JudgeRoute& route) {
    if (route.profile.has_value()) {
        return std::nullopt;
    }
    return route.id == llm::kJudgeProviderTypeSafe ? llm::openrouter_route()
                                                   : llm::typesafe_route();
}

std::string destination_copy(const llm::JudgeRoute& route) {
    if (route.is_local()) {
        return std::format("Transcript excerpts stay on this computer: {} answers here, even in "
                           "meetings with AI insights off.",
                           route.name);
    }
    if (route.profile.has_value()) {
        return std::format("Transcript excerpts go to {} ({}) only while AI insights are on for "
                           "the meeting.",
                           route.name, route.host());
    }
    if (route.id == llm::kJudgeProviderTypeSafe) {
        return std::format("Transcript excerpts go to TypeSafe ({}) only while AI insights are on "
                           "for the meeting.",
                           route.host());
    }
    return std::format("Transcript excerpts go to {} ({}), which passes them to TypeSafe, only "
                       "while AI insights are on for the meeting.",
                       route.name, route.host());
}

std::string text_model_label(const llm::JudgeRoute& route) {
    // OpenRouter ids carry the vendor; the profile already names the route.
    const std::size_t slash = route.model.rfind('/');
    const std::string model =
        slash == std::string::npos ? route.model : route.model.substr(slash + 1);
    return model.empty() ? route.name : std::format("{} · {}", route.name, model);
}

std::string rail_value(const Json& settings, bool key_present) {
    if (!keys::typesafe_enabled.read(settings)) {
        return "Off";
    }
    if (!key_present) {
        return "No key";
    }
    const bool flags[] = {
        keys::typesafe_topic_shift_enabled.read(settings),
        keys::typesafe_voice_commands_enabled.read(settings),
        keys::typesafe_citations_enabled.read(settings),
        keys::typesafe_semantic_search_enabled.read(settings),
        keys::typesafe_question_radar_enabled.read(settings),
        keys::typesafe_highlights_enabled.read(settings),
    };
    const auto active = std::count(std::begin(flags), std::end(flags), true);
    const std::string value =
        active == 0 ? std::string("On · no features") : std::format("On · {}", active);
    const llm::JudgeRoute route = llm::route_from_settings(settings);
    return route.id == llm::kJudgeProviderTypeSafe ? value
                                                   : std::format("{} · {}", value, route.name);
}

RouteView route_view(const Json& settings,
                     const std::function<bool(const llm::JudgeRoute&)>& key_present) {
    RouteView view;
    const llm::JudgeRoute route = llm::route_from_settings(settings);
    const std::vector<llm::JudgeRoute> candidates = routes(settings);
    view.route_index = std::max(0, indexOf(kRouteIds, route.id));
    for (const llm::JudgeRoute& candidate : candidates) {
        const char* state = key_present(candidate) ? "key set" : "no key";
        if (candidate.id == llm::kJudgeProviderTypeSafe) {
            view.route_details.push_back(std::format("Direct · {}", state));
        } else if (candidate.id == llm::kJudgeProviderOpenRouter) {
            view.route_details.push_back(std::format("Same model · {}", state));
        } else if (candidate.is_local()) {
            view.route_details.push_back(std::format("{} · on this PC", candidate.name));
        } else {
            view.route_details.push_back(std::format("{} · {}", candidate.name, state));
        }
    }
    view.text_model_visible = route.profile.has_value();
    view.text_model_index =
        std::max(0, indexOf(kTextModelIds, keys::typesafe_text_model.read(settings)));
    for (const std::string_view source : kTextModelIds) {
        Json patched = settings.is_object() ? settings : Json::object();
        patched[std::string(keys::typesafe_text_model.key())] = std::string(source);
        view.text_model_details.push_back(text_model_label(llm::text_model_route(patched)));
    }
    if (route.id == llm::kJudgeProviderTypeSafe) {
        view.description = std::format("Answers come straight from TypeSafe ({}) and are billed "
                                       "to your TypeSafe account.",
                                       route.host());
    } else if (route.id == llm::kJudgeProviderOpenRouter) {
        view.description = std::format("The same Jev model, reached through {0} ({1}) and billed "
                                       "to your {0} credits. No TypeSafe account needed.",
                                       route.name, route.host());
    } else {
        const std::string where =
            route.is_local() ? std::string("on this computer") : std::format("at {}", route.host());
        view.description = std::format(
            "{} answers {}, as set under AI cleanup or Meeting Mode → Intelligence. Slower than "
            "Jev and not benchmarked like it: a judgment that takes over {:.0f} s is skipped.",
            text_model_label(route), where, llm::kTextModelJudgeTimeoutS);
    }
    view.destination = destination_copy(route);
    return view;
}

KeyNotice key_notice(const llm::JudgeRoute& route, bool key_present, bool other_key_present,
                     bool master_on) {
    KeyNotice notice;
    const std::optional<llm::JudgeRoute> other = other_route(route);
    const bool other_present = other.has_value() && !key_present && other_key_present;
    notice.visible = !key_present;
    notice.switch_visible = other_present;
    // One blue action per banner: switching routes leads when it can.
    notice.add_tone = other_present ? "neutral" : "primary";
    if (other) {
        notice.switch_text = std::format("Use {}", other->name);
    }
    notice.add_tooltip = std::format("Open API keys with the {} credential selected", route.name);
    if (key_present) {
        return notice;
    }
    notice.title = std::format("No {}", route.key_label());
    const std::string situation =
        master_on ? "Nothing on this page runs without one, and these checks fail quietly by "
                    "design — no meeting will warn you."
                  : "Fast judgments are off and no key is set. Turning anything on below has no "
                    "effect until there is one.";
    const std::string remedy =
        other_present
            ? std::format("Your {} can run the same model instead, or add a key under API keys → "
                          "{}.",
                          other->key_label(), route.name)
            : std::format("Add a key under API keys → {}, or set {}.", route.name,
                          route.credential_env);
    notice.description = std::format("{} {}", situation, remedy);
    return notice;
}

} // namespace fast_judgments

// --- FastJudgmentsState ---

FastJudgmentsState::FastJudgmentsState(std::shared_ptr<JudgeKeys> keys, Executor& worker,
                                       Executor& ui)
    : keys_(std::move(keys)), worker_(worker), ui_(ui) {}

std::shared_ptr<FastJudgmentsState> FastJudgmentsState::create(std::shared_ptr<JudgeKeys> keys,
                                                               Executor& worker, Executor& ui) {
    std::shared_ptr<FastJudgmentsState> state(new FastJudgmentsState(std::move(keys), worker, ui));
    state->self_ = state;
    return state;
}

void FastJudgmentsState::read(std::vector<llm::JudgeRoute> routes, std::function<void()> done) {
    // One lookup per variable: the routes that share one (a text model on OpenRouter's key) ask
    // once.
    std::vector<llm::JudgeRoute> wanted;
    for (llm::JudgeRoute& route : routes) {
        if (route.credential_env.empty() ||
            std::ranges::any_of(wanted, [&](const llm::JudgeRoute& other) {
                return other.credential_env == route.credential_env;
            })) {
            continue;
        }
        ++pending_[route.credential_env];
        wanted.push_back(std::move(route));
    }
    const std::uint64_t token = ++requested_;
    // The worker holds no page and no widget: the state is plain data over services that outlive
    // the pages, and `done` (which may name a window) only runs on the GUI thread, where it
    // checks that the window is still there.
    worker_.post([weak = self_, token, wanted = std::move(wanted),
                  done = std::move(done)]() mutable {
        const std::shared_ptr<FastJudgmentsState> state = weak.lock();
        if (state == nullptr) {
            return;
        }
        auto answers = std::make_shared<std::vector<std::pair<std::string, bool>>>();
        for (const llm::JudgeRoute& route : wanted) {
            answers->emplace_back(route.credential_env, state->keys_->present(route));
        }
        state->ui_.post([weak, token, answers, done = std::move(done)] {
            const std::shared_ptr<FastJudgmentsState> alive = weak.lock();
            if (alive == nullptr) {
                return;
            }
            for (const auto& [name, present] : *answers) {
                if (const auto found = alive->pending_.find(name); found != alive->pending_.end()) {
                    if (--found->second <= 0) {
                        alive->pending_.erase(found);
                    }
                }
                // Reads may finish out of order on a wide worker: the newest one's answer wins.
                std::uint64_t& stamp = alive->stamps_[name];
                if (token > stamp) {
                    stamp = token;
                    alive->present_[name] = present;
                }
            }
            if (done) {
                done();
            }
        });
    });
}

bool FastJudgmentsState::present(const llm::JudgeRoute& route) const {
    if (route.credential_env.empty()) {
        return true;
    }
    const auto found = present_.find(route.credential_env);
    return found != present_.end() && found->second;
}

bool FastJudgmentsState::knows(const std::vector<llm::JudgeRoute>& routes) const {
    return std::ranges::all_of(routes, [this](const llm::JudgeRoute& route) {
        return route.credential_env.empty() || present_.contains(route.credential_env);
    });
}

bool FastJudgmentsState::reading(const std::vector<llm::JudgeRoute>& routes) const {
    return std::ranges::any_of(routes, [this](const llm::JudgeRoute& route) {
        return pending_.contains(route.credential_env);
    });
}

// --- FastJudgmentsPage ---

FastJudgmentsPage::FastJudgmentsPage(SettingsPage& page, SettingsDialog& dialog,
                                     std::shared_ptr<FastJudgmentsState> state)
    : QWidget(&page), dialog_(dialog), state_(std::move(state)),
      mailbox_(std::make_shared<detail::Mailbox<FastJudgmentsPage>>(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    build(page, layout);
}

FastJudgmentsPage::~FastJudgmentsPage() {
    mailbox_->close();
}

const std::vector<std::string>& FastJudgmentsPage::controlNames() {
    static const std::vector<std::string> names{
        "typesafe_key_notice",          "open_typesafe_key_btn",
        "typesafe_switch_route_btn",    "typesafe_route_tile",
        "typesafe_route_bar",           "typesafe_text_model_bar",
        "typesafe_route_test_btn",      "typesafe_route_status",
        "typesafe_enabled_tile",        "typesafe_enabled_check",
        "typesafe_topic_shift_tile",    "typesafe_topic_shift_check",
        "typesafe_voice_commands_tile", "typesafe_voice_commands_check",
        "typesafe_destination_label",
    };
    return names;
}

void FastJudgmentsPage::build(SettingsPage& page, QVBoxLayout* layout) {
    // Every switch on this page is inert without a key, and the features themselves degrade
    // silently by design, so the page has to say so.
    keyNotice_ =
        new InfoTile(QStringLiteral("No TypeSafe API key"), QString(), icon("info-warning.svg"));
    ui::setStyleProperty(keyNotice_, ui::prop::kind, QStringLiteral("notice"));
    openKeyButton_ = compactPrimaryButton(new Button(QStringLiteral("Add a key")));
    openKeyButton_->setObjectName(QStringLiteral("typesafeKeyNoticeLink"));
    openKeyButton_->setToolTip(
        QStringLiteral("Open API keys with the TypeSafe credential selected"));
    connect(openKeyButton_, &QPushButton::clicked, this,
            [this] { dialog_.focusApiKeys(q(selectedRoute().credential_env)); });
    // The quickest fix when the other route already has a key: one click moves Fast judgments
    // onto it. Shown only in that case.
    switchRouteButton_ = compactPrimaryButton(new Button(QStringLiteral("Use OpenRouter")));
    connect(switchRouteButton_, &QPushButton::clicked, this, [this] { switchRoute(); });
    switchRouteButton_->hide();
    keyNotice_->addActions({switchRouteButton_, openKeyButton_});
    // Hidden until the keys have been read: a key the store holds must not flash as missing.
    keyNotice_->hide();
    layout->addWidget(keyNotice_);

    // Jev either way for the first two, the choice being whose key pays and whether OpenRouter
    // sits in between; or one of the user's own text models, which can keep every judgment on
    // this computer.
    routeTile_ = new InfoTile(QStringLiteral("Judge"), QString(), icon("world-blue.svg"));
    std::vector<std::pair<QString, QString>> routeItems;
    for (const char* title : kRouteTitles) {
        routeItems.emplace_back(QString::fromLatin1(title), QString());
    }
    routeBar_ = new SegmentedBar(routeItems);
    routeBar_->setAccessibleName(QStringLiteral("Run fast judgments on"));
    connect(routeBar_, &SegmentedBar::activated, this,
            [this](int index) { onRouteActivated(index); });
    routeTile_->addBody(routeBar_);
    // The text models are set up on the Models page; this only picks one.
    std::vector<std::pair<QString, QString>> textModelItems;
    for (const char* title : kTextModelTitles) {
        textModelItems.emplace_back(QString::fromLatin1(title), QString());
    }
    textModelBar_ = new SegmentedBar(textModelItems, true);
    textModelBar_->setAccessibleName(QStringLiteral("Text model that answers fast judgments"));
    connect(textModelBar_, &SegmentedBar::activated, this,
            [this](int index) { onTextModelActivated(index); });
    textModelBar_->hide();
    routeTile_->addBody(textModelBar_);
    routeTestButton_ = neutralButton(new Button(QStringLiteral("Test")));
    routeTestButton_->setToolTip(
        QStringLiteral("Send one tiny judgment (the word “ok”) over this connection"));
    connect(routeTestButton_, &QPushButton::clicked, this, [this] { testRoute(); });
    routeTile_->addTrailing(routeTestButton_);
    routeStatus_ = new WrappedLabel(QString());
    // Shares the API keys page's warning and success colours.
    routeStatus_->setObjectName(QStringLiteral("apiKeyStatus"));
    routeStatus_->hide();
    routeTile_->addBody(routeStatus_);
    page.addTileGroup(layout, QString(), {routeTile_}, 1);

    enabledTile_ = new SettingTile(
        QStringLiteral("Live checks"),
        QStringLiteral("Answers narrow yes/no questions about a minute of transcript: about 0.2 s "
                       "with Jev, seconds with a text model. It never writes text."),
        icon("bolt-green.svg"));
    enabledTile_->addTitleBadge(QStringLiteral("Experimental"));
    enabledTile_->checkbox()->setObjectName(QStringLiteral("typesafeEnabledCheck"));

    SettingsBinder& bindings = dialog_.meetingBindings();
    topicShiftTile_ = new SettingTile(
        QStringLiteral("Semantic topic changes"),
        QStringLiteral("Fires early checkpoints on a judged topic change instead of word overlap, "
                       "and falls back to word overlap if no answer arrives."),
        icon("stack-purple.svg"));
    topicShiftTile_->addTitleBadge(QStringLiteral("Experimental"));
    topicShiftTile_->checkbox()->setObjectName(QStringLiteral("typesafeTopicShiftCheck"));
    bindings.checkbox(topicShiftTile_->checkbox(),
                      std::string(keys::typesafe_topic_shift_enabled.key()),
                      [](const Json& settings) {
                          return Json(keys::typesafe_topic_shift_enabled.read(settings));
                      });

    voiceCommandsTile_ = new SettingTile(
        QStringLiteral("Spoken instructions"),
        QStringLiteral("Say “Note taker, mark that as a decision”, “…add an action item” or "
                       "“…new topic: budget”. Only segments naming the assistant are judged. "
                       "“Replace X with Y” applies a reversible transcript correction."),
        icon("stack-slate.svg"));
    voiceCommandsTile_->addTitleBadge(QStringLiteral("Experimental"));
    voiceCommandsTile_->checkbox()->setObjectName(QStringLiteral("typesafeVoiceCommandsCheck"));
    bindings.checkbox(voiceCommandsTile_->checkbox(),
                      std::string(keys::typesafe_voice_commands_enabled.key()),
                      [](const Json& settings) {
                          return Json(keys::typesafe_voice_commands_enabled.read(settings));
                      });

    struct Feature {
        const char* feature;
        std::string_view key;
        const char* title;
        const char* description;
    };
    const Feature features[] = {
        {"citations", keys::typesafe_citations_enabled.key(), "Advisory citation checks",
         "Gives the judge generated claims and their cited excerpts. Flags weak evidence "
         "without changing the claim."},
        {"semantic_search", keys::typesafe_semantic_search_enabled.key(), "Semantic history search",
         "Gives the judge your search and shortlisted past excerpts to rank by meaning: only from "
         "AI-insight meetings unless it runs on this computer. Keyword search stays available."},
        {"question_radar", keys::typesafe_question_radar_enabled.key(), "Open questions radar",
         "Gives the judge a minute of transcript and tracked questions to find unanswered "
         "questions and suggest answers."},
        {"highlights", keys::typesafe_highlights_enabled.key(), "Live highlight pulses",
         "Gives the judge a minute of transcript to mark decisions, disagreements, dated "
         "commitments, numbers and takeaways. Click a pulse to play that moment."},
    };
    for (const Feature& feature : features) {
        auto* tile =
            new SettingTile(QString::fromUtf8(feature.title),
                            QString::fromUtf8(feature.description), icon("bolt-green.svg"));
        const std::string key(feature.key);
        bindings.checkbox(tile->checkbox(), key,
                          [key](const Json& settings) { return Json(storedTrue(settings, key)); });
        featureTiles_.emplace_back(feature.feature, tile);
    }
    std::vector<QWidget*> tiles{enabledTile_, topicShiftTile_, voiceCommandsTile_};
    for (const auto& [name, tile] : featureTiles_) {
        tiles.push_back(tile);
    }
    const SettingsPage::TileGroup group =
        page.addTileGroup(layout, QStringLiteral("Features"), std::move(tiles), 3,
                          q(fast_judgments::destination_copy(llm::typesafe_route())));
    destinationLabel_ = group.intro;

    connect(enabledTile_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool checked) { onEnabledChanged(checked); });
}

Json FastJudgmentsPage::settings() const {
    return *dialog_.settingsSnapshot();
}

llm::JudgeRoute FastJudgmentsPage::selectedRoute() const {
    return llm::route_from_settings(settings());
}

QString FastJudgmentsPage::railValue() const {
    const Json current = settings();
    return q(
        fast_judgments::rail_value(current, state_->present(llm::route_from_settings(current))));
}

bool FastJudgmentsPage::stateCurrent() const {
    const std::vector<llm::JudgeRoute> wanted = fast_judgments::routes(settings());
    return state_->knows(wanted) && !state_->reading(wanted);
}

QWidget* FastJudgmentsPage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"typesafe_key_notice", keyNotice_},
        {"open_typesafe_key_btn", openKeyButton_},
        {"typesafe_switch_route_btn", switchRouteButton_},
        {"typesafe_route_tile", routeTile_},
        {"typesafe_route_bar", routeBar_},
        {"typesafe_text_model_bar", textModelBar_},
        {"typesafe_route_test_btn", routeTestButton_},
        {"typesafe_route_status", routeStatus_},
        {"typesafe_enabled_tile", enabledTile_},
        {"typesafe_enabled_check", enabledTile_->checkbox()},
        {"typesafe_topic_shift_tile", topicShiftTile_},
        {"typesafe_topic_shift_check", topicShiftTile_->checkbox()},
        {"typesafe_voice_commands_tile", voiceCommandsTile_},
        {"typesafe_voice_commands_check", voiceCommandsTile_->checkbox()},
        {"typesafe_destination_label", destinationLabel_},
    };
    for (const auto& [control, widget] : controls) {
        if (control == name) {
            return widget;
        }
    }
    return nullptr;
}

void FastJudgmentsPage::loadSettings(const Json& settings) {
    enabledTile_->checkbox()->setChecked(keys::typesafe_enabled.read(settings));
    updateFeatureTiles();
}

void FastJudgmentsPage::refreshPage() {
    QPointer<SettingsDialog> dialog = &dialog_;
    state_->read(fast_judgments::routes(settings()), [dialog] {
        if (dialog != nullptr) {
            dialog->refreshRailValues();
        }
    });
}

// --- Actions ---

void FastJudgmentsPage::onEnabledChanged(bool checked) {
    dialog_.persist(keys::typesafe_enabled.key(), Json(checked));
    updateFeatureTiles();
}

void FastJudgmentsPage::onRouteActivated(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(fast_judgments::kRouteIds))) {
        return;
    }
    const std::string_view routeId = fast_judgments::kRouteIds[static_cast<std::size_t>(index)];
    if (routeId == selectedRoute().id) {
        return;
    }
    dialog_.persist(keys::typesafe_provider.key(), Json(std::string(routeId)));
    // A result for the other route would now describe the wrong thing.
    routeStatus_->hide();
    updateFeatureTiles();
}

void FastJudgmentsPage::onTextModelActivated(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(fast_judgments::kTextModelIds))) {
        return;
    }
    const std::string_view source = fast_judgments::kTextModelIds[static_cast<std::size_t>(index)];
    if (source == keys::typesafe_text_model.read(settings())) {
        return;
    }
    dialog_.persist(keys::typesafe_text_model.key(), Json(std::string(source)));
    routeStatus_->hide();
    updateFeatureTiles();
}

void FastJudgmentsPage::switchRoute() {
    const std::optional<llm::JudgeRoute> other = fast_judgments::other_route(selectedRoute());
    if (!other) {
        return;
    }
    // setCurrentIndex does not emit activated: the choice is made here, as a click would.
    const int index = indexOf(fast_judgments::kRouteIds, other->id);
    routeBar_->setCurrentIndex(index);
    onRouteActivated(routeBar_->currentIndex());
}

void FastJudgmentsPage::showRouteResult(std::optional<bool> ok, const QString& text) {
    routeStatus_->setText(text);
    ui::setStyleProperty(routeStatus_, ui::prop::tone,
                         ok.value_or(false) ? QStringLiteral("success")
                                            : QStringLiteral("warning"));
    routeStatus_->show();
}

void FastJudgmentsPage::testRoute() {
    if (testing_) {
        return;
    }
    const llm::JudgeRoute route = selectedRoute();
    const bool known = state_->knows({route}) && !state_->reading({route});
    if (known && !state_->present(route)) {
        // Python's route_key reads the environment and .env again on every Test; the store,
        // which blocks, was read last time. A key put in either since is tested, and the notice
        // and the rail are read again.
        if (!state_->keys()->presentOutsideStore(route)) {
            showRouteResult(false, noKeyText(route));
            return;
        }
        refreshPage();
        startRouteTest(route);
        return;
    }
    if (known) {
        startRouteTest(route);
        return;
    }
    // Not read yet, or being read again (a key just saved on the API keys page): the worker asks
    // the store first, as Python's synchronous route_key does, so a missing key is reported
    // without a "Testing…" line before it.
    testing_ = true;
    const std::uint64_t token = testGate_.next();
    qt::TokenGate gate = testGate_;
    state_->worker().post([mailbox = mailbox_, gate, token, route, keys = state_->keys()] {
        const std::optional<std::string> key = keys->key(route);
        const bool present = key.has_value() && !key->empty();
        mailbox->deliver(gate, token, [route, present](FastJudgmentsPage& page) {
            page.onRouteKeyRead(route, present);
        });
    });
}

QString FastJudgmentsPage::noKeyText(const llm::JudgeRoute& route) {
    return q(
        std::format("No {} to test. Add one under API keys → {}.", route.key_label(), route.name));
}

void FastJudgmentsPage::onRouteKeyRead(const llm::JudgeRoute& route, bool present) {
    testing_ = false;
    // A redraw while the key was looked up showed the button as busy.
    routeTestButton_->setEnabled(true);
    // The person switched routes while the key was being looked up.
    if (route != selectedRoute()) {
        return;
    }
    if (!present) {
        showRouteResult(false, noKeyText(route));
        return;
    }
    startRouteTest(route);
}

void FastJudgmentsPage::startRouteTest(const llm::JudgeRoute& route) {
    testing_ = true;
    routeTestButton_->setEnabled(false);
    showRouteResult(std::nullopt, q(std::format("Testing {}…", route.name)));
    const std::uint64_t token = testGate_.next();
    qt::TokenGate gate = testGate_;
    // Bound here: the worker must not reach back into the page. The key is read again there, so
    // one removed since is reported missing rather than sent.
    state_->worker().post(
        [mailbox = mailbox_, gate, token, route, keys = state_->keys()]() mutable {
            const std::optional<llm::VerifyResult> result = keys->verifyRoute(route);
            mailbox->deliver(gate, token, [route, result](FastJudgmentsPage& page) {
                page.onRouteVerified(route, result);
            });
        });
}

void FastJudgmentsPage::onRouteVerified(const llm::JudgeRoute& tested,
                                        std::optional<llm::VerifyResult> result) {
    testing_ = false;
    routeTestButton_->setEnabled(true);
    // The person switched routes while this one was being tested.
    const llm::JudgeRoute route = selectedRoute();
    if (tested != route) {
        return;
    }
    if (!result) {
        showRouteResult(false, noKeyText(route));
        return;
    }
    showRouteResult(result->ok, q(result->message));
}

// --- Rendering ---

void FastJudgmentsPage::renderRoute() {
    // The destination line is the page's last widget, so a refresh during construction waits for
    // the whole page.
    if (destinationLabel_ == nullptr) {
        return;
    }
    const FastJudgmentsState& state = *state_;
    const fast_judgments::RouteView view = fast_judgments::route_view(
        settings(), [&state](const llm::JudgeRoute& route) { return state.present(route); });
    routeBar_->setCurrentIndex(view.route_index);
    for (std::size_t i = 0; i < view.route_details.size(); ++i) {
        routeBar_->setDetail(static_cast<int>(i), q(view.route_details[i]));
    }
    textModelBar_->setVisible(view.text_model_visible);
    textModelBar_->setCurrentIndex(view.text_model_index);
    for (std::size_t i = 0; i < view.text_model_details.size(); ++i) {
        textModelBar_->setDetail(static_cast<int>(i), q(view.text_model_details[i]));
    }
    routeTile_->setDescription(q(view.description));
    const auto route = selectedRoute();
    destinationLabel_->setText(
        route.is_local()
            ? q(view.destination)
            : QStringLiteral("Use a text model on this computer for Meeting Fast Judgments. "
                             "The Test button checks the selected connection at %1.")
                  .arg(q(route.host())));
    routeTestButton_->setEnabled(!testing_);
}

void FastJudgmentsPage::renderKeyState() {
    // Rail refreshes can run before the page is built.
    if (destinationLabel_ == nullptr) {
        return;
    }
    renderRoute();
    const Json current = settings();
    if (!state_->knows(fast_judgments::routes(current))) {
        // Nothing is said about a key until the store has answered.
        keyNotice_->setVisible(false);
        switchRouteButton_->setVisible(false);
        return;
    }
    const llm::JudgeRoute route = llm::route_from_settings(current);
    const std::optional<llm::JudgeRoute> other = fast_judgments::other_route(route);
    const fast_judgments::KeyNotice notice = fast_judgments::key_notice(
        route, state_->present(route), other.has_value() && state_->present(*other),
        enabledTile_->checkbox()->isChecked());
    keyNotice_->setVisible(notice.visible);
    switchRouteButton_->setVisible(notice.switch_visible);
    ui::setStyleProperty(openKeyButton_, ui::prop::tone, q(notice.add_tone));
    if (notice.switch_text) {
        switchRouteButton_->setText(q(*notice.switch_text));
    }
    openKeyButton_->setToolTip(q(notice.add_tooltip));
    if (notice.title) {
        keyNotice_->titleLabel()->setText(q(*notice.title));
    }
    if (notice.description) {
        keyNotice_->setDescription(q(*notice.description));
    }
}

void FastJudgmentsPage::updateFeatureTiles() {
    const bool enabled = enabledTile_->checkbox()->isChecked();
    for (SettingTile* tile : {topicShiftTile_, voiceCommandsTile_}) {
        tile->setEnabled(enabled);
    }
    for (const auto& [name, tile] : featureTiles_) {
        tile->setEnabled(enabled);
    }
    renderKeyState();
    // The rail's value follows from the same state.
    dialog_.refreshRailValues();
}

// --- Registration ---

namespace {

// What the registered page and its rail value share, kept alive by their closures.
struct Registration {
    Registration(AppContext& app, FastJudgmentsOptions given)
        : context(app), options(std::move(given)) {}

    AppContext& context;
    FastJudgmentsOptions options;
    std::shared_ptr<FastJudgmentsState> state;
    QPointer<FastJudgmentsPage> page;
    // The window whose openings read the keys again.
    QPointer<SettingsDialog> watched;

    SettingsDialog* dialog() const {
        if (options.dialog) {
            return options.dialog();
        }
        SettingsWindow* window = context.find<SettingsWindow>();
        return window != nullptr ? window->dialog() : nullptr;
    }

    // Built on first use, from what the context has by then.
    FastJudgmentsState* ensureState() {
        if (state == nullptr && options.keys != nullptr) {
            try {
                Executor* worker = options.worker != nullptr ? options.worker : &context.io();
                state = FastJudgmentsState::create(options.keys, *worker, context.ui());
            } catch (const ServiceUnavailable&) {
                // No worker or UI executor yet: the page cannot read keys.
            }
        }
        return state.get();
    }

    Json currentSettings() const {
        try {
            return *context.settings().snapshot();
        } catch (const ServiceUnavailable&) {
            return Json::object();
        }
    }

    void read(const Json& settings) {
        FastJudgmentsState* keys = ensureState();
        if (keys == nullptr) {
            return;
        }
        keys->read(fast_judgments::routes(settings), [weak = self] {
            // The window the rail belongs to may be gone by the time the store has answered.
            if (const std::shared_ptr<Registration> alive = weak.lock()) {
                if (SettingsDialog* shown = alive->dialog()) {
                    shown->refreshRailValues();
                }
            }
        });
    }

    QString railValue(const Json& settings) {
        FastJudgmentsState* keys = ensureState();
        if (page != nullptr) {
            // Python redraws the page's key notice on every rail refresh, which is how a key
            // saved on another page reaches it.
            page->renderKeyState();
        }
        bool present = false;
        if (keys != nullptr) {
            const std::vector<llm::JudgeRoute> wanted = fast_judgments::routes(settings);
            if (!keys->knows(wanted) && !keys->reading(wanted)) {
                // The store blocks: read it on the worker and redraw once it has answered.
                read(settings);
            }
            present = keys->present(llm::route_from_settings(settings));
        }
        // Python's rail asks the store, the environment and .env on every redraw; here the
        // answers are kept, so each opening of the window reads them again (a key set in the
        // environment or .env since).
        if (SettingsDialog* window = dialog(); window != nullptr && window != watched) {
            watched = window;
            std::weak_ptr<Registration> weak = self;
            QObject::connect(window, &SettingsDialog::windowShown, window, [weak] {
                if (const std::shared_ptr<Registration> alive = weak.lock()) {
                    alive->read(alive->currentSettings());
                }
            });
        }
        return q(fast_judgments::rail_value(settings, present));
    }

    std::weak_ptr<Registration> self;
};

} // namespace

void registerFastJudgmentsPage(AppContext& context, FastJudgmentsOptions options) {
    if (options.keys == nullptr) {
        if (JudgeKeys* provided = context.find<JudgeKeys>()) {
            // A non-owning handle: the context keeps the service for as long as the page lives.
            options.keys = std::shared_ptr<JudgeKeys>(provided, [](JudgeKeys*) {});
        } else {
            try {
                options.keys = JudgeKeys::over(context);
            } catch (const ServiceUnavailable&) {
                kLog.debug("Fast judgments have no credential store to read keys from");
            }
        }
    }
    if (options.keys != nullptr && context.find<JudgeKeys>() == nullptr) {
        context.provide<JudgeKeys>(options.keys);
    }
    auto registration = std::make_shared<Registration>(context, std::move(options));
    registration->self = registration;

    // _on_api_keys_saved reaches the page through the rail in Python (the store is asked on every
    // redraw); here a saved or removed key reads them again.
    if (SettingsPagesSignals* signals = context.find<SettingsPagesSignals>()) {
        std::weak_ptr<Registration> weak = registration;
        QObject::connect(signals, &SettingsPagesSignals::apiKeysChanged, signals, [weak] {
            if (const std::shared_ptr<Registration> alive = weak.lock()) {
                alive->read(alive->currentSettings());
            }
        });
    }

    SettingsPageSpec spec;
    spec.destination = destinations::meeting_fast;
    spec.title = QStringLiteral("Fast judgments");
    spec.subtitle = QStringLiteral("Optional checks by Jev or your own text model. Each feature "
                                   "describes the text it shares.");
    spec.create = [registration](QWidget* parent) -> QWidget* {
        SettingsPage& page = pageOf(parent);
        FastJudgmentsState* state = registration->ensureState();
        if (state == nullptr) {
            throw std::logic_error("The Fast judgments page needs the credential store and a "
                                   "worker.");
        }
        auto* built = new FastJudgmentsPage(page, *page.dialog(), registration->state);
        registration->page = built;
        return built;
    };
    spec.railValue = [registration](const Json& settings) {
        return registration->railValue(settings);
    };
    spec.searchFields = {
        {"typesafe_key_notice", QStringLiteral("No TypeSafe API key"), QString()},
        {"typesafe_route_tile", QStringLiteral("Judge"),
         QStringLiteral("Run fast judgments on Jev with a TypeSafe key or through OpenRouter, or "
                        "on your own text model (the AI insights or AI cleanup one), which can "
                        "run on this computer with Ollama, LM Studio, vLLM, llama.cpp or "
                        "LocalAI. Test sends one tiny judgment.")},
        {"typesafe_enabled_tile", QStringLiteral("Live checks"),
         QStringLiteral("Experimental, off by default. Answers narrow yes/no questions about a "
                        "minute of transcript in about 0.2 s with Jev, seconds with a text "
                        "model; never writes text.")},
        {"typesafe_topic_shift_tile", QStringLiteral("Semantic topic changes"),
         QStringLiteral("Fire early checkpoints on a judged topic change instead of word "
                        "overlap. Doubled precision on human-labelled meetings; falls back to "
                        "word overlap when no answer arrives.")},
        {"typesafe_voice_commands_tile", QStringLiteral("Spoken instructions"),
         QStringLiteral("\"Note taker, mark that as a decision\", \"…add an action item\", "
                        "\"…put that in the notes\", \"…new topic: budget\". Only segments "
                        "naming the assistant are judged. Recap updates notes; \"replace X with "
                        "Y\" applies a reversible transcript correction.")},
    };
    spec.controls = FastJudgmentsPage::controlNames();
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
