// OpenAI-compatible transcription servers (tests/test_speech_endpoints.py): presets, settings,
// catalogs, requests. Catalogs and uploads go to a loopback server serving the committed replies
// of tests/fixtures/cloud.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../http/loopback_server.h"
#include "cloud_test_support.h"
#include "golden/golden.h"
#include "speech/api_models.h"
#include "speech/endpoints.h"
#include "speech/transcription_hints.h"

using namespace fv;
using namespace fv::speech;
using namespace fv::speech::test;
using fv::testing::LoopbackServer;

namespace {

const std::chrono::year_month_day kToday{std::chrono::year{2026}, std::chrono::month{11},
                                         std::chrono::day{15}};
const std::string kLanguage = "de-DE";
const std::vector<std::string> kPhrases{"Ksenia", "Kubernetes"};

std::shared_ptr<http::Client> direct_client() {
    return http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
}

// A loopback server answering routes[path] -> (status, body) as JSON, 404 {"detail": "Not
// Found"} for anything else (the `serve` helper).
LoopbackServer serve(std::map<std::string, std::pair<int, std::string>> routes) {
    return LoopbackServer([routes = std::move(routes)](const LoopbackServer::Request& request,
                                                       LoopbackServer::Reply& reply) {
        const auto found = routes.find(request.target);
        const auto& [status, body] =
            found != routes.end() ? found->second
                                  : std::pair<int, std::string>{404, cloud_reply("not_found.json")};
        reply.write(LoopbackServer::response(status, status == 200 ? "OK" : "Not Found",
                                             {{"Content-Type", "application/json"}}, body));
    });
}

SpeechEndpoint point(Json& settings, const std::string& id, const std::string& url,
                     const std::string& model = "m") {
    save_server(settings, id, url, "", model);
    return configured_endpoint(id, settings);
}

} // namespace

// ---- TestSelection ----

// from
// tests/test_speech_endpoints.py::TestSelection::test_existing_users_stay_on_openai_with_their_model
TEST_CASE("TestSelection::test_existing_users_stay_on_openai_with_their_model",
          "[test_speech_endpoints]") {
    Json settings = Json::object();
    save_model(settings, "openai", "gpt-4o-transcribe");
    CHECK(selected_endpoint(settings).id == kOpenAiEndpointId);
    CHECK_FALSE(selected_endpoint(settings).base_url);
    CHECK(selected_model(settings, kToday) == "gpt-4o-transcribe");
}

// from tests/test_speech_endpoints.py::TestSelection::test_an_unknown_endpoint_is_openai
TEST_CASE("TestSelection::test_an_unknown_endpoint_is_openai", "[test_speech_endpoints]") {
    for (const Json& saved : {Json("nonsense"), Json(42), Json(nullptr)}) {
        const Json settings{{"api_transcription_endpoint", saved}};
        CHECK(selected_endpoint(settings).id == kOpenAiEndpointId);
    }
}

// from
// tests/test_speech_endpoints.py::TestSelection::test_a_custom_slot_without_an_address_is_openai
TEST_CASE("TestSelection::test_a_custom_slot_without_an_address_is_openai",
          "[test_speech_endpoints]") {
    Json settings = Json::object();
    save_selection(settings, "custom");
    CHECK(selected_endpoint(settings).id == kOpenAiEndpointId);
}

// from tests/test_speech_endpoints.py::TestSelection::test_each_server_keeps_its_own_model
TEST_CASE("TestSelection::test_each_server_keeps_its_own_model", "[test_speech_endpoints]") {
    Json settings = Json::object();
    save_model(settings, "openai", "whisper-1");
    save_model(settings, "speaches", "Systran/faster-distil-whisper-small.en");
    save_selection(settings, "speaches");
    CHECK(selected_model(settings, kToday) == "Systran/faster-distil-whisper-small.en");
    save_selection(settings, "vllm");
    CHECK(selected_model(settings, kToday) == "openai/whisper-large-v3-turbo");
    save_selection(settings, "openai");
    CHECK(selected_model(settings, kToday) == "whisper-1");
}

// from tests/test_speech_endpoints.py::TestSelection::test_openai_takes_only_its_own_models
TEST_CASE("TestSelection::test_openai_takes_only_its_own_models", "[test_speech_endpoints]") {
    Json settings = Json::object();
    CHECK_THROWS_AS(save_model(settings, "openai", "whisper-large-v3"), std::invalid_argument);
    CHECK(settings.empty());
}

// from tests/test_speech_endpoints.py::TestSelection::test_hosted_services_have_fixed_addresses
TEST_CASE("TestSelection::test_hosted_services_have_fixed_addresses", "[test_speech_endpoints]") {
    Json settings = Json::object();
    CHECK_THROWS_AS(save_server(settings, "groq", "http://127.0.0.1:1/v1", "", "m"),
                    std::invalid_argument);
}

// ---- TestServerSettings ----

// from
// tests/test_speech_endpoints.py::TestServerSettings::test_saved_address_key_variable_and_model
TEST_CASE("TestServerSettings::test_saved_address_key_variable_and_model",
          "[test_speech_endpoints]") {
    Json settings = Json::object();
    save_server(settings, "speaches", "http://gpu-box:8000/", "SPEACHES_API_KEY",
                "Systran/faster-whisper-large-v3");
    const auto endpoint = configured_endpoint("speaches", settings);
    CHECK(endpoint.base_url == "http://gpu-box:8000/v1");
    CHECK(endpoint.api_key_env == "SPEACHES_API_KEY");
    CHECK(endpoint_model(endpoint, settings, kToday) == "Systran/faster-whisper-large-v3");
    CHECK_FALSE(endpoint.is_local());
}

// from tests/test_speech_endpoints.py::TestServerSettings::test_rejected_addresses
TEST_CASE("TestServerSettings::test_rejected_addresses", "[test_speech_endpoints]") {
    for (const char* url :
         {"ftp://host/v1", "http://user:pw@host/v1", "http://host/v1?key=1", ""}) {
        Json settings = Json::object();
        CHECK_THROWS_AS(save_server(settings, "custom", url, "", "m"), std::invalid_argument);
    }
}

// from
// tests/test_speech_endpoints.py::TestServerSettings::test_a_damaged_saved_server_falls_back_to_the_preset
TEST_CASE("TestServerSettings::test_a_damaged_saved_server_falls_back_to_the_preset",
          "[test_speech_endpoints]") {
    const Json settings{{"api_transcription_servers",
                         {{"vllm", {{"base_url", "not a url"}, {"api_key_env", "lower"}}}}}};
    CHECK(configured_endpoint("vllm", settings) == configured_endpoint("vllm", Json::object()));
}

// from
// tests/test_speech_endpoints.py::TestServerSettings::test_servers_the_user_runs_get_the_long_timeout
TEST_CASE("TestServerSettings::test_servers_the_user_runs_get_the_long_timeout",
          "[test_speech_endpoints]") {
    struct Row {
        const char* id;
        const char* url;
        bool self_hosted;
    };
    for (const Row& row : {Row{"openai", nullptr, false}, Row{"groq", nullptr, false},
                           Row{"speaches", "http://gpu-box:8000/v1", true},
                           Row{"custom", "http://127.0.0.1:9000/v1", true},
                           Row{"custom", "https://speech.example.com/v1", false}}) {
        Json settings = Json::object();
        if (row.url != nullptr) {
            save_server(settings, row.id, row.url, "", "m");
        }
        const auto endpoint = configured_endpoint(row.id, settings);
        CHECK(endpoint.self_hosted() == row.self_hosted);
        CHECK(endpoint.timeout_s() == (row.self_hosted ? kSelfHostedTimeoutS : kHostedTimeoutS));
    }
}

// from
// tests/test_speech_endpoints.py::TestServerSettings::test_custom_server_is_usable_once_it_has_an_address
TEST_CASE("TestServerSettings::test_custom_server_is_usable_once_it_has_an_address",
          "[test_speech_endpoints]") {
    Json settings = Json::object();
    save_server(settings, "custom", "http://127.0.0.1:9000/v1", "", "tiny");
    save_selection(settings, "custom");
    CHECK(selected_endpoint(settings).name == "Custom server");
    CHECK(selected_model(settings, kToday) == "tiny");
}

// from
// tests/test_speech_endpoints.py::TestServerSettings::test_only_endpoints_that_send_a_key_are_credential_owners
TEST_CASE("TestServerSettings::test_only_endpoints_that_send_a_key_are_credential_owners",
          "[test_speech_endpoints]") {
    Json settings = Json::object();
    save_server(settings, "localai", "http://127.0.0.1:8080/v1", "LOCALAI_API_KEY", "whisper-1");
    CHECK(credential_owners(settings) ==
          std::vector<std::pair<std::string, std::string>>{{"OPENAI_API_KEY", "OpenAI"},
                                                           {"GROQ_API_KEY", "Groq"},
                                                           {"LOCALAI_API_KEY", "LocalAI"}});
}

// ---- TestDestination ----

// from tests/test_speech_endpoints.py::TestDestination::test_names_where_audio_goes
TEST_CASE("TestDestination::test_names_where_audio_goes", "[test_speech_endpoints]") {
    const Json none = Json::object();
    CHECK(destination(configured_endpoint("openai", none)) == "OpenAI (api.openai.com)");
    CHECK(destination(configured_endpoint("groq", none)) == "Groq (api.groq.com)");
    const auto local = configured_endpoint("speaches", none);
    CHECK(local.is_local());
    CHECK(destination(local) == "your local server at 127.0.0.1:8000");
}

// from tests/test_speech_endpoints.py::TestDestination::test_privacy_sentence
TEST_CASE("TestDestination::test_privacy_sentence", "[test_speech_endpoints]") {
    const Json none = Json::object();
    CHECK(audio_destination_sentence(configured_endpoint("vllm", none)) ==
          "Audio stays on this computer: it goes to your local server at 127.0.0.1:8000.");
    CHECK(audio_destination_sentence(configured_endpoint("groq", none)) ==
          "Audio is uploaded to Groq (api.groq.com).");
}

// from
// tests/test_speech_endpoints.py::TestDestination::test_local_servers_need_no_key_but_hosted_ones_do
TEST_CASE("TestDestination::test_local_servers_need_no_key_but_hosted_ones_do",
          "[test_speech_endpoints]") {
    CloudRig rig;
    const Json none = Json::object();
    CHECK(api_key(configured_endpoint("whisper_cpp", none), rig.credentials, rig.env, rig.dotenv));
    CHECK_FALSE(api_key(configured_endpoint("groq", none), rig.credentials, rig.env, rig.dotenv));
    rig.credentials.set("GROQ_API_KEY", "gsk-test");
    CHECK(api_key(configured_endpoint("groq", none), rig.credentials, rig.env, rig.dotenv) ==
          "gsk-test");
}

// ---- TestRequestFields ----

// from
// tests/test_speech_endpoints.py::TestRequestFields::test_whisper_servers_take_a_prompt_and_one_language
TEST_CASE("TestRequestFields::test_whisper_servers_take_a_prompt_and_one_language",
          "[test_speech_endpoints]") {
    const auto speaches = configured_endpoint("speaches", Json::object());
    // Even a model named like OpenAI's newest takes Whisper fields elsewhere.
    for (const char* model : {"Systran/faster-whisper-small", "gpt-transcribe"}) {
        CHECK(transcription_hints(speaches, model, kLanguage, kPhrases).to_json() ==
              Json{{"prompt", "Ksenia, Kubernetes."}, {"language", "de"}});
        CHECK(response_format(speaches, model) == "json");
    }
}

// from
// tests/test_speech_endpoints.py::TestRequestFields::test_whisper_cpp_is_asked_to_detect_an_unknown_language
TEST_CASE("TestRequestFields::test_whisper_cpp_is_asked_to_detect_an_unknown_language",
          "[test_speech_endpoints]") {
    const auto server = configured_endpoint("whisper_cpp", Json::object());
    CHECK(transcription_hints(server, "whisper-1", "", {}).to_json() == Json{{"language", "auto"}});
    CHECK(transcription_hints(server, "whisper-1", kLanguage, kPhrases).language == "de");
}

// from tests/test_speech_endpoints.py::TestRequestFields::test_openai_fields_are_unchanged
TEST_CASE("TestRequestFields::test_openai_fields_are_unchanged", "[test_speech_endpoints]") {
    const auto endpoint = configured_endpoint("openai", Json::object());
    CHECK(response_format(endpoint, "gpt-transcribe") == "json");
    CHECK(response_format(endpoint, "whisper-1") == "text");
    CHECK(transcription_hints(endpoint, "whisper-1", "", {}).empty());
}

// ---- TestCatalogs ----

// from tests/test_speech_endpoints.py::TestCatalogs::test_speaches_lists_only_its_speech_models
TEST_CASE("TestCatalogs::test_speaches_lists_only_its_speech_models", "[test_speech_endpoints]") {
    auto server = serve({{"/v1/models", {200, cloud_reply("speaches_models.json")}}});
    Json settings = Json::object();
    const auto endpoint = point(settings, "speaches", server.url("/v1"));
    CloudRig rig;
    const auto key = api_key(endpoint, rig.credentials, rig.env, rig.dotenv);
    const auto models = list_models(*direct_client(), endpoint, key, {}, kToday);
    CHECK(models == std::vector<std::string>{"Systran/faster-distil-whisper-large-v3",
                                             "Systran/faster-whisper-small"});
    // No key configured: the placeholder, which an open server ignores.
    const auto seen = server.requests();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].header("Authorization") == "Bearer " + *key);
    CHECK(*key == kAuthFreeApiKey);
}

// from tests/test_speech_endpoints.py::TestCatalogs::test_localai_lists_models_that_transcribe
TEST_CASE("TestCatalogs::test_localai_lists_models_that_transcribe", "[test_speech_endpoints]") {
    auto server =
        serve({{"/v1/models/capabilities", {200, cloud_reply("localai_capabilities.json")}}});
    Json settings = Json::object();
    const auto endpoint = point(settings, "localai", server.url("/v1"));
    CHECK(list_models(*direct_client(), endpoint, std::string(kAuthFreeApiKey), {}, kToday) ==
          std::vector<std::string>{"whisper-1"});
}

// from
// tests/test_speech_endpoints.py::TestCatalogs::test_groq_offers_its_whisper_models_without_asking
TEST_CASE("TestCatalogs::test_groq_offers_its_whisper_models_without_asking",
          "[test_speech_endpoints]") {
    http::FakeHttpClient http;
    CHECK(
        list_models(http, configured_endpoint("groq", Json::object()), std::nullopt, {}, kToday) ==
        std::vector<std::string>{"whisper-large-v3-turbo", "whisper-large-v3"});
    CHECK(http.requests().empty());
}

// from tests/test_speech_endpoints.py::TestCatalogs::test_a_server_without_a_catalog_lists_nothing
TEST_CASE("TestCatalogs::test_a_server_without_a_catalog_lists_nothing",
          "[test_speech_endpoints]") {
    auto server = serve({});
    Json settings = Json::object();
    CHECK(list_models(*direct_client(), point(settings, "vllm", server.url("/v1")),
                      std::string(kAuthFreeApiKey), {}, kToday)
              .empty());
    http::FakeHttpClient http;
    CHECK(list_models(http, configured_endpoint("whisper_cpp", Json::object()), std::nullopt, {},
                      kToday)
              .empty());
    CHECK(http.requests().empty());
}

// from tests/test_speech_endpoints.py::TestCatalogs::test_openai_lists_its_curated_models
TEST_CASE("TestCatalogs::test_openai_lists_its_curated_models", "[test_speech_endpoints]") {
    http::FakeHttpClient http;
    CHECK(list_models(http, configured_endpoint("openai", Json::object()), std::nullopt, {}, kToday)
              .front() == "gpt-transcribe");
}

TEST_CASE("a catalog that fails says so to the caller", "[speech_endpoints]") {
    http::FakeHttpClient http;
    Json settings = Json::object();
    const auto endpoint = point(settings, "speaches", "http://127.0.0.1:8000/v1");
    http.push(json_reply(500, R"({"detail": "boom"})"));
    CHECK_THROWS(list_models(http, endpoint, std::nullopt, {}, kToday));
    http.push(http::ScriptedResponse{.error = http::Error{http::ErrorKind::Connect, "refused"}});
    CHECK_THROWS(list_models(http, endpoint, std::nullopt, {}, kToday));
    http.push(json_reply(200, "not json"));
    CHECK_THROWS(list_models(http, endpoint, std::nullopt, {}, kToday));
    // Without a key no Authorization header is sent.
    REQUIRE(http.requests().size() == 3);
    CHECK_FALSE(http::find_header(http.requests()[0].headers, "Authorization"));
}

// ---- TestBackend ----

// from tests/test_speech_endpoints.py::TestBackend::test_a_selected_server_gets_its_model_and_json
TEST_CASE("TestBackend::test_a_selected_server_gets_its_model_and_json",
          "[test_speech_endpoints]") {
    CloudRig rig;
    rig.http.set_handler(
        [](const http::Request&) { return json_reply(200, R"({"text": " hallo "})"); });
    rig.stub_splits({});
    auto& backend = rig.make("api");
    rig.apply([](Json& settings) {
        save_server(settings, "speaches", "http://127.0.0.1:8000/v1", "",
                    "Systran/faster-whisper-small");
        save_selection(settings, "speaches");
    });
    const auto audio = rig.root.path() / "a.wav";
    write_file(audio, "RIFF");
    CHECK(backend.decode(file_job(audio, kLanguage, kPhrases)).transcript.text == "hallo");
    const auto sent = rig.http.multipart_parts();
    REQUIRE(sent.size() == 1);
    const SentForm form = form_of(sent[0]);
    CHECK(form.field("model") == "Systran/faster-whisper-small");
    CHECK(form.field("response_format") == "json");
    CHECK(form.field("prompt") == "Ksenia, Kubernetes.");
    CHECK(form.field("language") == "de");
    CHECK(rig.http.requests()[0].url == "http://127.0.0.1:8000/v1/audio/transcriptions");
    CHECK(backend.name() == "Speaches (Systran/faster-whisper-small)");
}

// from
// tests/test_speech_endpoints.py::TestBackend::test_a_missing_model_on_another_server_is_not_swapped_for_openais
TEST_CASE("TestBackend::test_a_missing_model_on_another_server_is_not_swapped_for_openais",
          "[test_speech_endpoints]") {
    CloudRig rig;
    rig.http.set_handler([](const http::Request&) {
        return json_reply(404, R"({"detail": "Model 'whisper-1' is not installed locally."})");
    });
    rig.stub_splits({});
    auto& backend = rig.make("api");
    rig.apply([](Json& settings) {
        point(settings, "speaches", "http://127.0.0.1:8000/v1", "whisper-1");
        save_selection(settings, "speaches");
    });
    const auto audio = rig.root.path() / "a.wav";
    write_file(audio, "RIFF");
    CHECK_THROWS_AS(backend.decode(file_job(audio)), SpeechApiError);
    std::vector<std::string> models;
    for (const auto& parts : rig.http.multipart_parts()) {
        models.push_back(form_of(parts).field("model"));
    }
    CHECK(models == std::vector<std::string>{"whisper-1"});
}

// from
// tests/test_speech_endpoints.py::TestBackend::test_a_server_needing_an_unset_key_is_unavailable
TEST_CASE("TestBackend::test_a_server_needing_an_unset_key_is_unavailable",
          "[test_speech_endpoints]") {
    CloudRig rig;
    auto& backend = rig.make("api");
    rig.apply([](Json& settings) {
        save_server(settings, "vllm", "http://127.0.0.1:8000/v1", "VLLM_API_KEY", "m");
        save_selection(settings, "vllm");
    });
    CHECK_FALSE(backend.is_available());
    rig.credentials.set("VLLM_API_KEY", "token");
    CHECK(backend.is_available());
}

// from tests/test_speech_endpoints.py::test_worker_sends_to_the_chosen_server
TEST_CASE("test_worker_sends_to_the_chosen_server", "[test_speech_endpoints]") {
    // Adapted: "the backend sends to the chosen server". A server the user runs gets the upload
    // at its own address with the long read timeout; OpenAI's has no address of its own.
    CloudRig rig;
    rig.http.set_handler(
        [](const http::Request&) { return json_reply(200, R"({"text": "done"})"); });
    rig.stub_splits({});
    auto& backend = rig.make("api");
    const auto audio = rig.root.path() / "a.wav";
    write_file(audio, "RIFF");
    rig.apply([](Json& settings) {
        save_server(settings, "speaches", "http://127.0.0.1:8000/v1", "", "m");
        save_selection(settings, "speaches");
    });
    backend.decode(file_job(audio));
    rig.apply([](Json& settings) { save_selection(settings, "openai"); });
    backend.decode(file_job(audio));
    const auto requests = rig.http.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].url == "http://127.0.0.1:8000/v1/audio/transcriptions");
    CHECK(requests[0].idle_timeout == std::chrono::milliseconds(600'000));
    CHECK(requests[0].connect_timeout == std::chrono::milliseconds(10'000));
    CHECK(requests[1].url == "https://api.openai.com/v1/audio/transcriptions");
    CHECK(requests[1].idle_timeout == std::chrono::milliseconds(120'000));
}

// from tests/test_speech_endpoints.py::test_isolated_upload_reaches_a_local_server
TEST_CASE("test_isolated_upload_reaches_a_local_server", "[test_speech_endpoints]") {
    // A real HTTP client and a loopback server shaped like Speaches.
    auto server =
        serve({{"/v1/audio/transcriptions", {200, cloud_reply("speaches_transcription.json")}}});
    CloudRig rig;
    const auto client = direct_client();
    rig.stub_splits({});
    auto& backend = rig.make("api", false, client.get());
    rig.apply([&server](Json& settings) {
        point(settings, "speaches", server.url("/v1"), "Systran/faster-whisper-small");
        save_selection(settings, "speaches");
    });
    const auto audio = rig.root.path() / "a.wav";
    write_file(audio, "RIFF" + std::string(64, '0'));
    CHECK(backend.decode(file_job(audio)).transcript.text == "from the local server");
    const auto seen = server.requests();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].method == "POST");
    CHECK(seen[0].target == "/v1/audio/transcriptions");
    CHECK(seen[0].body.find("name=\"model\"\r\n\r\nSystran/faster-whisper-small\r\n") !=
          std::string::npos);
    CHECK(seen[0].body.find("name=\"response_format\"\r\n\r\njson\r\n") != std::string::npos);
}

// ---- golden vectors ----

namespace {

Json fields_of(const SpeechEndpoint& endpoint) {
    return Json{{"id", endpoint.id},
                {"name", endpoint.name},
                {"kind", endpoint.kind},
                {"base_url", endpoint.base_url ? Json(*endpoint.base_url) : Json(nullptr)},
                {"api_key_env", endpoint.api_key_env},
                {"builtin", endpoint.builtin},
                {"default_model", endpoint.default_model},
                {"configurable", endpoint.configurable},
                {"description", endpoint.description},
                {"models_path", endpoint.models_path ? Json(*endpoint.models_path) : Json(nullptr)},
                {"known_models", endpoint.known_models},
                {"auto_language", endpoint.auto_language},
                {"is_openai", endpoint.is_openai()},
                {"is_local", endpoint.is_local()},
                {"is_configured", endpoint.is_configured()},
                {"self_hosted", endpoint.self_hosted()},
                {"timeout_s", endpoint.timeout_s()}};
}

std::chrono::year_month_day iso_day(const std::string& text) {
    using namespace std::chrono;
    return year_month_day{year{std::stoi(text.substr(0, 4))},
                          month{static_cast<unsigned>(std::stoi(text.substr(5, 2)))},
                          day{static_cast<unsigned>(std::stoi(text.substr(8, 2)))}};
}

// The id a case names, as the Python dict lookup sees it (a non-string is no endpoint's).
std::string id_of(const golden::Case& c, std::size_t index) {
    const Json id = c.arg<Json>(index);
    return id.is_string() ? id.get<std::string>() : std::string("\x01not-an-id");
}

// A save_* case: the edited copy, or a ValueError.
void check_save(const golden::Case& c, const std::function<void(Json&)>& save) {
    Json settings = c.arg<Json>(0);
    try {
        save(settings);
        REQUIRE_FALSE(c.raises());
        CHECK(golden::equal(c.result<Json>(), settings));
    } catch (const std::invalid_argument&) {
        CHECK(c.raises_type() == "ValueError");
    }
}

} // namespace

TEST_CASE("the endpoint functions match Python over settings dicts", "[golden]") {
    const std::string module = "services.speech_endpoints";
    golden::for_each(module, "configured_endpoint", [&](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            fields_of(configured_endpoint(id_of(c, 0), c.arg<Json>(1)))));
    });
    golden::for_each(module, "selected_endpoint", [&](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), fields_of(selected_endpoint(c.arg<Json>(0)))));
    });
    golden::for_each(module, "endpoint_model", [&](const golden::Case& c) {
        const Json settings = c.arg<Json>(1);
        CHECK(endpoint_model(configured_endpoint(id_of(c, 0), settings), settings,
                             iso_day(c.arg<std::string>(2))) == c.result<std::string>());
    });
    golden::for_each(module, "selected_model", [&](const golden::Case& c) {
        CHECK(selected_model(c.arg<Json>(0), iso_day(c.arg<std::string>(1))) ==
              c.result<std::string>());
    });
    const auto string_or_raise = [](const golden::Case& c,
                                    const std::function<std::string()>& call) {
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(call(), std::invalid_argument);
        } else {
            CHECK(call() == c.result<std::string>());
        }
    };
    golden::for_each(module, "valid_server_url", [&](const golden::Case& c) {
        string_or_raise(c, [&] { return valid_server_url(c.arg<std::string>(0)); });
    });
    golden::for_each(module, "normalize_base_url", [&](const golden::Case& c) {
        string_or_raise(c, [&] { return normalize_base_url(c.arg<std::string>(0)); });
    });
    golden::for_each(module, "validate_api_key_env", [&](const golden::Case& c) {
        string_or_raise(c, [&] { return validate_api_key_env(c.arg<std::string>(0)); });
    });
    golden::for_each(module, "is_loopback_url", [&](const golden::Case& c) {
        CHECK(is_loopback_url(c.arg<std::optional<std::string>>(0)) == c.result<bool>());
    });
    golden::for_each(module, "destination", [&](const golden::Case& c) {
        CHECK(destination(configured_endpoint(id_of(c, 0), c.arg<Json>(1))) ==
              c.result<std::string>());
    });
    golden::for_each(module, "audio_destination_sentence", [&](const golden::Case& c) {
        CHECK(audio_destination_sentence(configured_endpoint(id_of(c, 0), c.arg<Json>(1))) ==
              c.result<std::string>());
    });
    golden::for_each(module, "credential_owners", [&](const golden::Case& c) {
        Json owners = Json::array();
        for (const auto& [variable, name] : credential_owners(c.arg<Json>(0))) {
            owners.push_back(Json::array({variable, name}));
        }
        CHECK(golden::equal(c.result<Json>(), owners));
    });
    golden::for_each(module, "_is_speech_model", [&](const golden::Case& c) {
        CHECK(is_speech_model(c.arg<Json>(0)) == c.result<bool>());
    });
    golden::for_each(module, "save_selection", [&](const golden::Case& c) {
        check_save(c, [&](Json& settings) { save_selection(settings, c.arg<std::string>(1)); });
    });
    golden::for_each(module, "save_model", [&](const golden::Case& c) {
        check_save(c, [&](Json& settings) {
            save_model(settings, c.arg<std::string>(1), c.arg<std::string>(2));
        });
    });
    golden::for_each(module, "save_server", [&](const golden::Case& c) {
        check_save(c, [&](Json& settings) {
            save_server(settings, c.arg<std::string>(1), c.arg<std::string>(2),
                        c.arg<std::string>(3), c.arg<std::string>(4));
        });
    });
    golden::for_each(module, "list_models", [&](const golden::Case& c) {
        http::FakeHttpClient http;
        CHECK(list_models(http, configured_endpoint(id_of(c, 0), c.arg<Json>(1)), std::nullopt, {},
                          iso_day(c.arg<std::string>(2))) == c.result<std::vector<std::string>>());
        CHECK(http.requests().empty());
    });
}
