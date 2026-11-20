// M4 end to end (Acceptance of M4.14): the real feature graph with the Personalize feature,
// offscreen, over fakes, and an OpenAI-compatible text model this test serves on 127.0.0.1. The
// fake microphone plays the tone fixture, the fake engine says what each scenario scripts, the
// focus capture names the app a scenario is in, and the synthetic keys' copy puts the scenario's
// selection on the test process's own (offscreen) clipboard. Every request the endpoint gets is
// recorded, so a test reads the system prompt AI cleanup, a profile or a style sent.
#include <catch2/catch_test_macros.hpp>

#include "../http/loopback_server.h"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

#include "appkit/destinations.h"
#include "dictation/hotkey_runtime.h"
#include "focus/catalog.h"
#include "hotkeys/key_event.h"
#include "http/client.h"
#include "personalize_support.h"
#include "settings/keys.h"
#include "sql/db.h"
#include "text/app_styles.h"
#include "text/cleanup_profiles.h"
#include "ui_personalize/cleanup_rules_section.h"
#include "ui_personalize/rules_page.h"

using fv::Json;
using fv::testing::LoopbackServer;
using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

namespace dest = fv::destinations;
using namespace std::chrono_literals;

constexpr char kModel[] = "test-model";

// The text model: answers Chat Completions with what `reply` makes of the request, and keeps every
// request's JSON body.
class TextModel {
public:
    TextModel()
        : server_([this](const LoopbackServer::Request& request, LoopbackServer::Reply& reply) {
              answer(request, reply);
          }) {}

    std::string baseUrl() const { return server_.url("/v1"); }

    // The reply for a request body; the transcript (the last user message) echoed when unset.
    void setReply(std::function<std::string(const Json& body)> reply) {
        std::lock_guard lock(mutex_);
        reply_ = std::move(reply);
    }
    std::vector<Json> bodies() const {
        std::lock_guard lock(mutex_);
        return bodies_;
    }
    std::size_t count() const {
        std::lock_guard lock(mutex_);
        return bodies_.size();
    }

    static std::string systemPrompt(const Json& body) {
        for (const Json& message : body.at("messages")) {
            if (message.value("role", "") == "system") {
                return message.value("content", "");
            }
        }
        return {};
    }
    static std::string userText(const Json& body) {
        std::string text;
        for (const Json& message : body.at("messages")) {
            if (message.value("role", "") == "user") {
                text = message.value("content", "");
            }
        }
        return text;
    }

private:
    void answer(const LoopbackServer::Request& request, LoopbackServer::Reply& reply) {
        if (request.method != "POST" ||
            request.target.find("/chat/completions") == std::string::npos) {
            reply.write(LoopbackServer::response(404, "Not Found", {}, "{}"));
            return;
        }
        Json body = Json::parse(request.body, nullptr, false);
        std::string content;
        {
            std::lock_guard lock(mutex_);
            bodies_.push_back(body);
            content = reply_ ? reply_(body) : userText(body);
        }
        const Json response{
            {"id", "chatcmpl-test"},
            {"object", "chat.completion"},
            {"model", kModel},
            {"choices",
             Json::array({Json{{"index", 0},
                               {"message", Json{{"role", "assistant"}, {"content", content}}},
                               {"finish_reason", "stop"}}})}};
        reply.write(LoopbackServer::response(200, "OK", {{"Content-Type", "application/json"}},
                                             response.dump()));
    }

    mutable std::mutex mutex_;
    std::function<std::string(const Json&)> reply_;
    std::vector<Json> bodies_;
    LoopbackServer server_;
};

// The app with focus: named per scenario, with the caret text a reread finds.
class SwitchableCapture final : public fv::focus::CaptureService {
public:
    void setApp(std::string appId, std::string name) {
        std::lock_guard lock(mutex_);
        identity_.app_id = std::move(appId);
        identity_.name = std::move(name);
        identity_.platform = "win32";
    }
    // What a reread of the field finds before the caret.
    void setField(std::string before) {
        std::lock_guard lock(mutex_);
        field_ = std::move(before);
    }
    std::shared_ptr<fv::focus::FocusRequest> request(bool, bool) override {
        fv::focus::FocusSnapshot snapshot;
        std::lock_guard lock(mutex_);
        snapshot.identity = identity_;
        fv::focus::TextContext text;
        text.caret_known = true;
        text.selection_known = false;
        text.source = "test";
        snapshot.text = text;
        return fv::focus::FocusRequest::resolved(snapshot);
    }
    std::optional<fv::focus::AppIdentity> current_identity() override {
        std::lock_guard lock(mutex_);
        return identity_;
    }
    void reread(const std::optional<fv::focus::AppIdentity>&,
                std::function<void(std::optional<fv::focus::TextContext>)> callback) override {
        fv::focus::TextContext text;
        {
            std::lock_guard lock(mutex_);
            text.before = field_;
        }
        text.caret_known = true;
        text.source = "test";
        callback(text);
    }
    void shutdown() override {}

private:
    std::mutex mutex_;
    fv::focus::AppIdentity identity_;
    std::string field_;
};

// What the clipboard held at each paste (text and its HTML flavour).
struct Paste {
    QString text;
    QString html;
};

// The M4 rig: Personalize linked, AI cleanup on at Medium through the test's text model, a
// dictionary word, a snippet, app styles on with Work messages casual, and the scripted focus.
class M4Rig {
public:
    explicit M4Rig(std::vector<std::string> heard, Json extra = Json::object())
        : http_(fv::http::make_curl_client(fv::http::CurlClientOptions{[](std::string_view) {
              return std::optional<std::string>();
          }})),
          capture_(std::make_shared<SwitchableCapture>()) {
        capture_->setApp("notepad.exe", "Notepad");
        Json settings = fvtest::customEndpoint(model.baseUrl(), kModel);
        settings["transcript_cleanup_enabled"] = true;
        settings["transcript_cleanup_level"] = "medium";
        settings["dictation_dictionary"] = Json::array({Json{{"id", "ksenia"},
                                                             {"term", "Ksenia"},
                                                             {"starred", false},
                                                             {"heard", Json::array({"Ksenya"})},
                                                             {"learned", false},
                                                             {"new", false}}});
        settings["dictation_snippets"] = Json::array({Json{{"id", "sig"},
                                                           {"trigger", "my sign-off"},
                                                           {"text", "**Dana Lee**\nDesigner"},
                                                           {"formatted", true}}});
        Json tones = Json::object();
        for (const std::string_view category : fv::text::app_styles::CATEGORIES) {
            tones[std::string(category)] = "formal";
        }
        tones[std::string(fv::text::app_styles::WORK)] = "casual";
        settings["app_context_enabled"] = true;
        settings["app_styles_enabled"] = true;
        settings["app_style_tones"] = tones;
        for (const auto& [key, value] : extra.items()) {
            settings[key] = value;
        }
        fvtest::RigOptions options = fvtest::personalizeOptions(keys_, http_.get(), settings);
        options.script.texts = std::move(heard);
        auto capture = capture_;
        options.platform = [capture](fv::ControllerPlatform& platform) {
            platform.captureService = [capture](const fv::SettingsStore&) {
                return capture;
            };
        };
        rig_ = std::make_unique<ControllerRig>(std::move(options));
        rig_->keys().on_send = [this](const std::string& what) {
            QClipboard* clipboard = QGuiApplication::clipboard();
            if (what == "paste") {
                const QMimeData* data = clipboard->mimeData();
                std::lock_guard lock(mutex_);
                pastes_.push_back(Paste{clipboard->text(), data != nullptr && data->hasHtml()
                                                               ? data->html()
                                                               : QString()});
            } else if (what == "copy" && selection) {
                clipboard->setText(QString::fromStdString(*selection));
            }
        };
    }
    ~M4Rig() { rig_.reset(); }
    M4Rig(const M4Rig&) = delete;
    M4Rig& operator=(const M4Rig&) = delete;

    ControllerRig& rig() { return *rig_; }
    SwitchableCapture& capture() { return *capture_; }
    std::vector<Paste> pastes() const {
        std::lock_guard lock(mutex_);
        return pastes_;
    }

    // A whole dictation on the record shortcut (kp *): pressed, 1.5 s of the tone, pressed again,
    // and back to idle with a paste.
    void dictate() {
        ControllerRig& r = *rig_;
        const std::size_t before = pastes().size();
        r.pressKeypad("*");
        REQUIRE(waitUntil([&r] { return r.runtime().is_recording(); }));
        REQUIRE(waitUntil([&r] { return !r.audio().streams().empty(); }));
        CHECK(r.speak(1.5) > 0);
        // The record shortcut ignores a press within 300 ms of the last one.
        waitUntil([] { return false; }, 400ms);
        r.pressKeypad("*");
        REQUIRE(waitUntil(
            [this, &r, before] {
                return pastes().size() > before && !r.runtime().has_active_job();
            },
            30s));
    }

    // One History row's text and cleanup columns, newest first.
    struct Row {
        std::string text, raw_text, provider, model, level, kind, source;
    };
    std::vector<Row> history() {
        fv::Database& database = rig_->database();
        return fvtest::offGui([&database] {
            return database.read([](fv::sql::Connection& connection) {
                const auto text = [](const fv::sql::Value& value) {
                    const auto* held = std::get_if<std::string>(&value);
                    return held != nullptr ? *held : std::string();
                };
                std::vector<Row> rows;
                auto statement = connection.prepare(
                    "SELECT text, raw_text, cleanup_provider, cleanup_model, cleanup_level, "
                    "entry_kind, source_name FROM transcription_history ORDER BY rowid DESC");
                while (statement.step()) {
                    rows.push_back(Row{text(statement.column(0)), text(statement.column(1)),
                                       text(statement.column(2)), text(statement.column(3)),
                                       text(statement.column(4)), text(statement.column(5)),
                                       text(statement.column(6))});
                }
                return rows;
            });
        });
    }

    TextModel model;
    // The selected text a copy finds; nullopt copies nothing.
    std::optional<std::string> selection;

private:
    fvtest::MemoryKeys keys_;
    std::shared_ptr<fv::http::Client> http_;
    std::shared_ptr<SwitchableCapture> capture_;
    std::unique_ptr<ControllerRig> rig_;
    mutable std::mutex mutex_;
    std::vector<Paste> pastes_;
};

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string styleBlock(std::string_view category, std::string_view tone) {
    fv::text::app_styles::AppStyle style;
    style.category = std::string(category);
    style.tone = std::string(tone);
    return fv::text::app_styles::prompt_block(style);
}

} // namespace

TEST_CASE("M4: a dictation is cleaned up at Medium, spelled by the dictionary, and saved",
          "[e2e][widget]") {
    M4Rig m4({"um so meet ksenya tomorrow at ten"});
    m4.model.setReply([](const Json&) { return std::string("Meet Ksenia tomorrow at 10."); });
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());

    m4.dictate();

    REQUIRE(m4.model.count() == 1);
    const Json request = m4.model.bodies().front();
    CHECK(request.value("model", "") == kModel);
    // The dictionary's spelling reaches the model (prepare_text runs before cleanup), and the
    // Medium instructions go with it.
    CHECK(TextModel::userText(request) == "um so meet Ksenia tomorrow at ten");
    CHECK_FALSE(TextModel::systemPrompt(request).empty());
    // The paste is the model's text.
    REQUIRE(m4.pastes().size() == 1);
    CHECK(m4.pastes().front().text == QStringLiteral("Meet Ksenia tomorrow at 10."));
    // History records the cleanup: the text before it, the endpoint, the model and the level.
    const auto rows = m4.history();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].text == "Meet Ksenia tomorrow at 10.");
    CHECK(rows[0].raw_text == "um so meet Ksenia tomorrow at ten");
    CHECK(rows[0].provider == "custom_e2e00001");
    CHECK(rows[0].model == kModel);
    CHECK(rows[0].level == "medium");
}

TEST_CASE("M4: a snippet's trigger pastes the snippet, as text and as rich text", "[e2e][widget]") {
    M4Rig m4({"my sign-off"});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());

    m4.dictate();

    REQUIRE(m4.pastes().size() == 1);
    const Paste paste = m4.pastes().front();
    CHECK(paste.text == QStringLiteral("Dana Lee\nDesigner"));
    CHECK(paste.html.contains(QStringLiteral("Dana Lee")));
    const bool bold = paste.html.contains(QStringLiteral("<b>")) ||
                      paste.html.contains(QStringLiteral("<strong>")) ||
                      paste.html.contains(QStringLiteral("font-weight"));
    CHECK(bold);
}

TEST_CASE("M4: the app's writing style reaches the model's instructions", "[e2e][widget]") {
    M4Rig m4({"see you at the standup", "see you at the standup"});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());

    m4.capture().setApp("slack.exe", "Slack");
    m4.dictate();
    m4.capture().setApp("outlook.exe", "Outlook");
    waitUntil([] { return false; }, 400ms);
    m4.dictate();

    const std::vector<Json> requests = m4.model.bodies();
    REQUIRE(requests.size() == 2);
    const std::string casual = styleBlock(fv::text::app_styles::WORK, "casual");
    REQUIRE_FALSE(casual.empty());
    // Formal is the cleanup prompt's own voice: Outlook's instructions add no style block.
    CHECK(styleBlock(fv::text::app_styles::EMAIL, "formal").empty());
    const std::string slack = TextModel::systemPrompt(requests[0]);
    const std::string outlook = TextModel::systemPrompt(requests[1]);
    CHECK(contains(slack, casual));
    CHECK_FALSE(contains(outlook, casual));
    CHECK_FALSE(outlook.empty());
}

TEST_CASE("M4: a profile's shortcut formats with the profile's prompt", "[e2e][widget]") {
    M4Rig m4({"ask about the invoice from march"});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());
    m4.model.setReply([](const Json&) { return std::string("Subject: Invoice\n\nHello,"); });

    rig.hotkeys().profile_record_requested.emit("email");
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(waitUntil([&rig] { return !rig.audio().streams().empty(); }));
    CHECK(rig.speak(1.5) > 0);
    rig.hotkeys().profile_record_requested.emit("email");
    REQUIRE(waitUntil(
        [&m4, &rig] { return !m4.pastes().empty() && !rig.runtime().has_active_job(); }, 30s));

    REQUIRE(m4.model.count() == 1);
    const std::string prompt = TextModel::systemPrompt(m4.model.bodies().front());
    const auto email = fv::text::find_cleanup_profile(*rig.settings().snapshot(), "email");
    REQUIRE(email.has_value());
    CHECK(contains(prompt, email->instructions));
    CHECK(m4.pastes().back().text == QStringLiteral("Subject: Invoice\n\nHello,"));
}

TEST_CASE("test_holding_the_command_shortcut_rewrites_the_selection_and_pastes", "[e2e][widget]") {
    // Held, the shortcut records until it is released (the push-and-hold trigger).
    // Auto-paste is off; a rewrite pastes anyway.
    M4Rig m4({"make it shorter"},
             Json{{"recording_trigger_mode", "push_hold"}, {"auto_paste", false}});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());
    m4.selection = "This paragraph is much longer than it needs to be, honestly.";
    m4.model.setReply([](const Json&) { return std::string("This is too long."); });

    const double pressed = fv::hotkeys::monotonic_clock()();
    rig.hotkeys().queue_command_press(pressed);
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(waitUntil([&rig] { return !rig.audio().streams().empty(); }));
    CHECK(rig.speak(1.5) > 0);
    // The blocks arrive at once; a release under 250 ms of the press cancels (RECORD_MIN_HOLD_MS).
    waitUntil([] { return false; }, 400ms);
    rig.hotkeys().queue_command_release(fv::hotkeys::monotonic_clock()());
    const bool pasted = waitUntil(
        [&m4, &rig] { return !m4.pastes().empty() && !rig.runtime().has_active_job(); }, 30s);
    std::string said;
    for (const std::string& status : rig.statuses()) {
        said += status + " | ";
    }
    INFO("statuses: " << said);
    REQUIRE(pasted);

    REQUIRE(m4.model.count() == 1);
    const Json request = m4.model.bodies().front();
    const std::string sent = request.dump();
    CHECK(contains(sent, "make it shorter"));
    CHECK(contains(sent, "much longer than it needs to be"));
    CHECK(m4.pastes().back().text == QStringLiteral("This is too long."));
    CHECK(rig.keys().sent() == std::vector<std::string>{"copy", "paste"});
    REQUIRE(waitUntil([&m4] { return !m4.history().empty(); }));
    const auto rows = m4.history();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].text == "This is too long.");
    CHECK(rows[0].raw_text == "This paragraph is much longer than it needs to be, honestly.");
    CHECK(rows[0].kind == "command");
    CHECK(rows[0].source == "Command Mode");
    CHECK_FALSE(rig.runtime().has_active_job());
}

TEST_CASE("test_a_transform_shortcut_rewrites_the_selection", "[e2e][widget]") {
    M4Rig m4({"unused"});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());
    m4.selection = "teh quick brown fox";
    m4.model.setReply([](const Json&) { return std::string("The quick brown fox."); });

    rig.hotkeys().transform_requested.emit("polish");
    REQUIRE(waitUntil(
        [&m4, &rig] { return !m4.pastes().empty() && !rig.runtime().has_active_job(); }, 30s));

    REQUIRE(m4.model.count() == 1);
    CHECK(contains(m4.model.bodies().front().dump(), "teh quick brown fox"));
    CHECK(m4.pastes().back().text == QStringLiteral("The quick brown fox."));
    REQUIRE(waitUntil([&m4] { return !m4.history().empty(); }));
    const auto rows = m4.history();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == "transform");
    CHECK(rows[0].source == "Transform · Polish");
    CHECK(rows[0].raw_text == "teh quick brown fox");
}

TEST_CASE("M4: a dictated rule is polished by the model and saved", "[e2e][widget]") {
    M4Rig m4({"always spell my name alex rivera"});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());
    m4.model.setReply(
        [](const Json&) { return std::string("Always spell my name \"Alex Rivera\"."); });
    // The polished rule is reviewed only while Settings shows.
    fv::SettingsDialog& dialog = fvtest::settingsDialog(rig);
    dialog.show();
    dialog.selectDestination(QString::fromUtf8(dest::cleanup_rules.data()));
    auto* page = dynamic_cast<fv::RulesPage*>(dialog.pageContent(dest::cleanup_rules));
    REQUIRE(page != nullptr);
    fv::CleanupRulesSection& rules = *page->section();
    rules.setDictationPath(rig.paths().data_root() / "rule_dictation.wav");
    std::vector<fv::RuleReview> reviews;
    rules.reviewRule = [&reviews](const fv::RuleReview& review, QWidget*) {
        reviews.push_back(review);
        return std::optional<QString>(review.rule);
    };

    rules.toggleDictation();
    REQUIRE(rules.dictationState() == fv::CleanupRulesSection::DictationState::Recording);
    CHECK(rig.speak(1.5) > 0);
    rules.toggleDictation();
    REQUIRE(waitUntil([&reviews] { return !reviews.empty(); }, 30s));

    CHECK(reviews.front().dictated);
    CHECK(reviews.front().rule == QStringLiteral("Always spell my name \"Alex Rivera\"."));
    REQUIRE(m4.model.count() == 1);
    CHECK(contains(m4.model.bodies().front().dump(), "always spell my name alex rivera"));
    const Json saved = fv::keys::transcript_cleanup_rules.read(*rig.settings().snapshot());
    CHECK(saved == Json::array({"Always spell my name \"Alex Rivera\"."}));
    dialog.hide();
}

TEST_CASE("M4: a word cleanup changed and the user put back is learned and said so",
          "[e2e][widget]") {
    M4Rig m4({"deploy it with kubectl now"},
             Json{{"app_context_read_text", true}, {"dictionary_learn_enabled", true}});
    ControllerRig& rig = m4.rig();
    REQUIRE(rig.startEngine());
    m4.model.setReply([](const Json&) { return std::string("Deploy it with kubernetes now."); });
    // The user puts the word back in the field after the paste.
    m4.capture().setField("Deploy it with kubectl now.");

    m4.dictate();

    REQUIRE(m4.pastes().size() == 1);
    CHECK(m4.pastes().front().text == QStringLiteral("Deploy it with kubernetes now."));
    // The field is read again 4 s after the paste (LEARN_DELAY).
    REQUIRE(waitUntil(
        [&rig] {
            return rig.window().status() ==
                   QStringLiteral("AI cleanup will keep \"kubectl\" as you said it");
        },
        15s));
    bool learned = false;
    for (const Json& word : fv::keys::dictation_dictionary.read(*rig.settings().snapshot())) {
        learned = learned || word.value("term", "") == "kubectl";
    }
    CHECK(learned);
}
