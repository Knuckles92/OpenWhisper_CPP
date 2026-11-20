// The API backend's Server and Model fields and the server editor. The catalog requests go
// through a FakeHttpClient on a ManualExecutor, so a test decides when the worker runs.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "http/testing/fake_http_client.h"
#include "settings/testing/memory_store.h"
#include "speech/endpoints.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/field_help.h"
#include "ui_widgets/speech_server_controls.h"
#include "ui_widgets/speech_server_dialog.h"

namespace {

using fv::Json;
using fv::SpeechServerControls;
using fv::SpeechServerDialog;

// A settings store, the fake network and the manual io executor of one test. Every field a test
// makes is declared after it, so the fields go first.
struct Rig {
    explicit Rig(const Json& saved = Json::object()) : settings(saved) {
        SpeechServerControls::resetCatalogs();
        client.set_handler([this](const fv::http::Request& request) {
            asked.push_back(request.url);
            fv::http::ScriptedResponse response;
            if (failure) {
                response.error = *failure;
                return response;
            }
            Json data = Json::array();
            for (const std::string& id : models) {
                data.push_back(Json{{"id", id}});
            }
            response.body = Json{{"data", data}}.dump();
            return response;
        });
    }
    ~Rig() { SpeechServerControls::resetCatalogs(); }

    // Queues the answer to the next catalog request.
    void respond(const std::vector<std::string>& ids) {
        Json data = Json::array();
        for (const std::string& id : ids) {
            data.push_back(Json{{"id", id}});
        }
        fv::http::ScriptedResponse response;
        response.body = Json{{"data", data}}.dump();
        client.push(std::move(response));
    }
    // Runs the queued catalog requests, then delivers their results to the widgets.
    void settle() {
        io.run_all();
        QApplication::processEvents();
    }

    fv::MemorySettingsStore settings;
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
    fv::FakeClock clock;
    std::vector<std::string> asked;
    std::vector<std::string> models{"Systran/faster-whisper-large-v3",
                                    "Systran/faster-whisper-small"};
    std::optional<fv::http::Error> failure;
};

std::vector<QString> items(const QComboBox* combo) {
    std::vector<QString> data;
    for (int index = 0; index < combo->count(); ++index) {
        data.push_back(combo->itemData(index).toString());
    }
    return data;
}

std::vector<QString> strings(std::initializer_list<const char*> values) {
    std::vector<QString> made;
    for (const char* value : values) {
        made.push_back(QString::fromUtf8(value));
    }
    return made;
}

void choose(QComboBox* combo, const char* data) {
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(data)));
}

bool editEnabled(const QComboBox* combo) {
    const auto* model = qobject_cast<const QStandardItemModel*>(combo->model());
    return model->item(combo->findData(QString::fromLatin1(SpeechServerControls::kEditOption)))
        ->isEnabled();
}

// An editor that fills the dialog and saves it, as a person pressing Save server would.
SpeechServerControls::EditorRunner answer(const char* url, const char* model, bool accept = true) {
    return [=](SpeechServerDialog& dialog) {
        if (!accept) {
            return false;
        }
        dialog.urlEdit()->setText(QString::fromUtf8(url));
        dialog.modelEdit()->setText(QString::fromUtf8(model));
        dialog.save();
        return dialog.result() == QDialog::Accepted;
    };
}

} // namespace

// from tests/test_speech_server_controls.py::test_openai_is_shown_with_its_models
TEST_CASE("openai is shown with its models", "[test_speech_server_controls][widget]") {
    Rig rig;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    controls.setClock(rig.clock);
    controls.refresh();
    CHECK(controls.serverCombo()->currentData().toString() == QStringLiteral("openai"));
    const std::vector<QString> servers = items(controls.serverCombo());
    const std::vector<std::string> ids = fv::speech::endpoint_ids();
    REQUIRE(servers.size() >= ids.size());
    for (std::size_t index = 0; index < ids.size(); ++index) {
        CHECK(servers[index].toStdString() == ids[index]);
    }
    CHECK(controls.serverCombo()->itemText(controls.serverCombo()->findData(
              QStringLiteral("custom"))) == QStringLiteral(u"Custom…"));
    CHECK(controls.modelCombo()->currentData().toString() == QStringLiteral("gpt-transcribe"));
    CHECK(controls.modelCombo()
              ->itemText(controls.modelCombo()->findData(QStringLiteral("whisper-1")))
              .contains(QStringLiteral("whisper-1 (retiring")));
    CHECK_FALSE(editEnabled(controls.serverCombo()));
    CHECK(controls.serverCombo()->toolTip().contains(
        QStringLiteral("uploaded to OpenAI (api.openai.com)")));
    // OpenAI's models are curated: nothing was asked of the network.
    rig.settle();
    CHECK(rig.asked.empty());
}

// from
// tests/test_speech_server_controls.py::test_picking_a_local_server_saves_it_and_lists_its_models
TEST_CASE("picking a local server saves it and lists its models",
          "[test_speech_server_controls][widget]") {
    Rig rig;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    QSignalSpy changes(&controls, &SpeechServerControls::changed);
    choose(controls.serverCombo(), "speaches");
    CHECK(rig.settings.contents().at("api_transcription_endpoint") == "speaches");
    CHECK(changes.count() == 1);
    // The preset's default model stays listed until the server answers.
    CHECK(items(controls.modelCombo()) == strings({"Systran/faster-whisper-small"}));
    rig.settle();
    CHECK(items(controls.modelCombo()) ==
          strings({"Systran/faster-whisper-large-v3", "Systran/faster-whisper-small"}));
    CHECK(controls.modelCombo()->currentData().toString() ==
          QStringLiteral("Systran/faster-whisper-small"));
    CHECK(rig.asked == std::vector<std::string>{"http://127.0.0.1:8000/v1/models"});
    CHECK(controls.serverCombo()->toolTip().contains(QStringLiteral("stays on this computer")));

    choose(controls.modelCombo(), "Systran/faster-whisper-large-v3");
    const Json saved = rig.settings.contents();
    CHECK(saved.at("api_transcription_servers").at("speaches").at("model") ==
          "Systran/faster-whisper-large-v3");
    CHECK_FALSE(saved.contains("api_transcription_model"));
    CHECK(changes.count() == 2);
    CHECK(editEnabled(controls.serverCombo()));
}

// from
// tests/test_speech_server_controls.py::test_a_late_catalog_for_another_server_is_kept_but_not_shown
TEST_CASE("a late catalog for another server is kept but not shown",
          "[test_speech_server_controls][widget]") {
    Rig rig;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    choose(controls.serverCombo(), "speaches");
    choose(controls.serverCombo(), "groq");
    // The Speaches request only runs now, after Groq is the selection.
    rig.settle();
    CHECK(SpeechServerControls::catalogCount() == 1);
    CHECK(rig.asked.size() == 1);
    CHECK(items(controls.modelCombo()) == strings({"whisper-large-v3-turbo", "whisper-large-v3"}));
}

// from tests/test_speech_server_controls.py::test_an_unreachable_server_says_so
TEST_CASE("an unreachable server says so", "[test_speech_server_controls][widget]") {
    Rig rig;
    rig.failure = fv::http::Error{fv::http::ErrorKind::Connect, "refused"};
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    choose(controls.serverCombo(), "vllm");
    rig.settle();
    CHECK(controls.modelCombo()->toolTip().contains(QStringLiteral("Couldn't reach vLLM")));
    CHECK(controls.modelCombo()->toolTip().contains(QStringLiteral("http://127.0.0.1:8000/v1")));
    CHECK(items(controls.modelCombo()) == strings({"openai/whisper-large-v3-turbo"}));
    // A failed request keeps nothing: the next pick asks again.
    CHECK(SpeechServerControls::catalogCount() == 0);
}

// from tests/test_speech_server_controls.py::test_custom_without_an_address_asks_for_one
TEST_CASE("custom without an address asks for one", "[test_speech_server_controls][widget]") {
    Rig rig;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    controls.setClock(rig.clock);
    controls.setEditorRunner(answer("", "", false));
    choose(controls.serverCombo(), "custom");
    CHECK(controls.serverCombo()->currentData().toString() == QStringLiteral("openai"));
    CHECK_FALSE(rig.settings.contents().contains("api_transcription_endpoint"));

    controls.setEditorRunner(answer("http://10.0.0.5:9000", "large-v3"));
    choose(controls.serverCombo(), "custom");
    CHECK(controls.serverCombo()->currentData().toString() == QStringLiteral("custom"));
    CHECK(controls.serverCombo()->currentText() == QStringLiteral("Custom server"));
    const Json saved = rig.settings.contents();
    CHECK(fv::speech::selected_endpoint(saved).base_url == "http://10.0.0.5:9000/v1");
    CHECK(fv::speech::selected_model(
              saved, std::chrono::year_month_day{std::chrono::year{2026}, std::chrono::month{1},
                                                 std::chrono::day{1}}) == "large-v3");
    CHECK(controls.serverCombo()->toolTip().contains(
        QStringLiteral("uploaded to Custom server (10.0.0.5:9000)")));
}

// from
// tests/test_speech_server_controls.py::test_edit_server_changes_the_address_and_keeps_the_selection
TEST_CASE("edit server changes the address and keeps the selection",
          "[test_speech_server_controls][widget]") {
    Rig rig(Json{{"api_transcription_endpoint", "whisper_cpp"}});
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    QSignalSpy changes(&controls, &SpeechServerControls::changed);
    controls.setEditorRunner(answer("http://127.0.0.1:9090/v1", "ggml-large-v3"));
    choose(controls.serverCombo(), SpeechServerControls::kEditOption);
    CHECK(controls.serverCombo()->currentData().toString() == QStringLiteral("whisper_cpp"));
    CHECK(fv::speech::selected_endpoint(rig.settings.contents()).base_url ==
          "http://127.0.0.1:9090/v1");
    CHECK(controls.modelCombo()->currentData().toString() == QStringLiteral("ggml-large-v3"));
    CHECK(changes.count() == 1);
}

// from tests/test_speech_server_controls.py::test_dialog_reports_a_bad_address
TEST_CASE("dialog reports a bad address", "[test_speech_server_controls][widget]") {
    Rig rig;
    SpeechServerDialog dialog(rig.settings,
                              fv::speech::configured_endpoint("localai", rig.settings.contents()));
    dialog.urlEdit()->setText(QStringLiteral("http://user:secret@host/v1"));
    dialog.save();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.errorLabel()->text().contains(QStringLiteral("without credentials")));
    CHECK_FALSE(dialog.saved().has_value());
    CHECK_FALSE(rig.settings.contents().contains("api_transcription_servers"));
}

TEST_CASE("the editor shows the server and saves its fields", "[widget]") {
    Rig rig(Json{{"api_transcription_servers", {{"vllm", {{"model", "my/model"}}}}}});
    SpeechServerDialog dialog(rig.settings,
                              fv::speech::configured_endpoint("vllm", rig.settings.contents()));
    CHECK(dialog.objectName() == QStringLiteral("speechServerDialog"));
    CHECK(dialog.windowTitle() == QStringLiteral("vLLM server"));
    CHECK(dialog.minimumWidth() == 460);
    CHECK(dialog.isModal());
    CHECK(dialog.urlEdit()->text() == QStringLiteral("http://127.0.0.1:8000/v1"));
    CHECK(dialog.modelEdit()->text() == QStringLiteral("my/model"));
    CHECK(dialog.envEdit()->text().isEmpty());
    CHECK(dialog.saveButton()->property("buttonId").toString() ==
          QStringLiteral("speechServerSaveButton"));

    dialog.urlEdit()->setText(QStringLiteral("http://10.1.1.1:9000"));
    dialog.envEdit()->setText(QStringLiteral("VLLM_KEY"));
    dialog.save();
    CHECK(dialog.result() == QDialog::Accepted);
    REQUIRE(dialog.saved().has_value());
    CHECK(dialog.saved()->baseUrl == "http://10.1.1.1:9000/v1");
    CHECK(dialog.saved()->apiKeyEnv == "VLLM_KEY");
    CHECK(dialog.saved()->model == "my/model");
}

TEST_CASE("choosing a server asks it again, and the key provider supplies its key", "[widget]") {
    Rig rig;
    std::vector<std::string> keysAskedFor;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    controls.setKeyProvider([&](const fv::speech::SpeechEndpoint& endpoint) {
        keysAskedFor.push_back(endpoint.id);
        return std::optional<std::string>("secret");
    });
    choose(controls.serverCombo(), "speaches");
    rig.settle();
    const auto sent = rig.client.requests();
    REQUIRE(sent.size() == 1);
    const auto& headers = sent.front().headers;
    CHECK(std::ranges::find(headers, std::make_pair(std::string("Authorization"),
                                                    std::string("Bearer secret"))) !=
          headers.end());
    CHECK(keysAskedFor == std::vector<std::string>{"speaches"});

    // A refresh uses what the session holds; picking the server again re-fetches.
    controls.refresh();
    rig.settle();
    CHECK(rig.asked.size() == 1);
    choose(controls.serverCombo(), "vllm");
    choose(controls.serverCombo(), "speaches");
    rig.settle();
    CHECK(rig.asked.size() == 3);
}

TEST_CASE("a key provider set after the first request asks again with the key", "[widget]") {
    Rig rig(Json{{"api_transcription_endpoint", "speaches"}});
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    // The composer sets the provider after construction, when the first request already went out
    // without a key: the server is asked again with it, and that answer is shown.
    controls.setKeyProvider(
        [](const fv::speech::SpeechEndpoint&) { return std::optional<std::string>("secret"); });
    rig.settle();
    const auto sent = rig.client.requests();
    REQUIRE(sent.size() == 2);
    const auto authorized = [](const fv::http::Request& request) {
        return std::ranges::find(request.headers, std::make_pair(std::string("Authorization"),
                                                                 std::string("Bearer secret"))) !=
               request.headers.end();
    };
    CHECK_FALSE(authorized(sent[0]));
    CHECK(authorized(sent[1]));
    CHECK(items(controls.modelCombo()) ==
          strings({"Systran/faster-whisper-large-v3", "Systran/faster-whisper-small"}));

    // With the catalog held, or for a server without a catalog route, a new provider asks nothing.
    controls.setKeyProvider({});
    rig.settle();
    CHECK(rig.client.requests().size() == 2);
    choose(controls.serverCombo(), "groq");
    controls.setKeyProvider({});
    rig.settle();
    CHECK(rig.client.requests().size() == 2);
}

TEST_CASE("a request that outlives its widget touches nothing", "[widget]") {
    Rig rig;
    {
        SpeechServerControls controls(rig.settings, rig.client, rig.io);
        choose(controls.serverCombo(), "speaches");
    }
    // The worker runs after the widget was destroyed: the widget cancelled the request, and
    // neither the request nor its result may reach the widget.
    rig.settle();
    CHECK(SpeechServerControls::catalogCount() == 0);
    // The next field for that server asks for itself.
    rig.models = {"Systran/faster-whisper-small"};
    SpeechServerControls next(rig.settings, rig.client, rig.io);
    choose(next.serverCombo(), "speaches");
    rig.settle();
    CHECK(SpeechServerControls::catalogCount() == 1);
}

TEST_CASE("the newest catalog request is the one shown", "[widget]") {
    Rig rig(Json{{"api_transcription_endpoint", "speaches"}});
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    // Two requests are queued: the constructor's, and the one a refresh makes. Each is answered
    // in turn; the second's list is the one shown.
    rig.respond({"old"});
    rig.respond({"new"});
    controls.refresh();
    rig.settle();
    // Queued answers bypass the handler that fills `asked`.
    CHECK(rig.client.requests().size() == 2);
    CHECK(items(controls.modelCombo()) == strings({"Systran/faster-whisper-small", "new"}));
}

TEST_CASE("busy disables both fields", "[widget]") {
    Rig rig;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    controls.setBusy(true);
    CHECK_FALSE(controls.serverCombo()->isEnabled());
    CHECK_FALSE(controls.modelCombo()->isEnabled());
    controls.setBusy(false);
    CHECK(controls.serverCombo()->isEnabled());
}

TEST_CASE("the help links name the API keys page", "[widget]") {
    Rig rig;
    SpeechServerControls controls(rig.settings, rig.client, rig.io);
    QSignalSpy requested(&controls, &SpeechServerControls::helpRequested);
    const auto buttons = controls.serverField()->findChildren<QPushButton*>();
    REQUIRE_FALSE(buttons.isEmpty());
    // The caption is a FieldHelp: its link button names the destination.
    auto* help = qobject_cast<fv::FieldHelp*>(buttons.front());
    REQUIRE(help != nullptr);
    REQUIRE(help->links().size() == 1);
    help->links().front()->click();
    REQUIRE(requested.count() == 1);
    CHECK(requested.front().front().toString() == QStringLiteral("api_keys"));
}
