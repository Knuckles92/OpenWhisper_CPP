// Transcription hints (tests/test_personalize_s4_openai.py): per-model fields, the one retry
// without them, chunks, and the fields on the wire.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../http/loopback_server.h"
#include "base/log.h"
#include "cloud_test_support.h"
#include "golden/golden.h"
#include "speech/api_models.h"
#include "speech/transcription_hints.h"

using namespace fv;
using namespace fv::speech;
using namespace fv::speech::test;
using fv::testing::LoopbackServer;

namespace {

const std::string kLanguage = "de-DE";
const std::vector<std::string> kPhrases{"Ksenia", "Kubernetes"};

SpeechEndpoint openai() {
    return configured_endpoint(kOpenAiEndpointId, Json::object());
}

using Fields = std::map<std::string, std::vector<std::string>>;

// The hint fields as they go on the wire: lists under their bracketed names.
Fields wire_fields(const TranscriptionHints& hints) {
    Fields fields;
    if (hints.keywords) {
        fields["keywords[]"] = *hints.keywords;
    }
    if (hints.languages) {
        fields["languages[]"] = *hints.languages;
    }
    if (hints.prompt) {
        fields["prompt"] = {*hints.prompt};
    }
    if (hints.language) {
        fields["language"] = {*hints.language};
    }
    return fields;
}

// Counts warnings that reach the log, keeping their text.
class WarningSink final : public log::Sink {
public:
    void write(const log::Record& record) override {
        if (record.level >= log::Level::Warning) {
            std::lock_guard lock(mutex_);
            text_ += std::string(record.message) + "\n";
        }
    }
    std::string text() {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    std::mutex mutex_;
    std::string text_;
};

http::ScriptedResponse rejected(int status, std::optional<std::string> code = std::nullopt) {
    Json error{{"message", "HTTP " + std::to_string(status)}};
    if (code) {
        error["code"] = *code;
    }
    return json_reply(status, Json{{"error", error}}.dump());
}

// The backend of TestRequests: an OpenAI key, `model`, no splitting.
CloudSpeechBackend& requests_backend(CloudRig& rig, const std::string& model) {
    rig.stub_splits({});
    return rig.make(model);
}

} // namespace

// ---- TestHintFields ----

// from
// tests/test_personalize_s4_openai.py::TestHintFields::test_gpt_transcribe_takes_keywords_and_languages
TEST_CASE("TestHintFields::test_gpt_transcribe_takes_keywords_and_languages",
          "[test_personalize_s4_openai]") {
    const auto hints = transcription_hints(openai(), "gpt-transcribe", kLanguage, kPhrases);
    CHECK(hints.to_json() == Json{{"keywords", {"Ksenia", "Kubernetes"}}, {"languages", {"de"}}});
}

// from
// tests/test_personalize_s4_openai.py::TestHintFields::test_older_models_take_a_prompt_and_language
TEST_CASE("TestHintFields::test_older_models_take_a_prompt_and_language",
          "[test_personalize_s4_openai]") {
    for (const char* model : {"whisper-1", "gpt-4o-transcribe", "gpt-4o-mini-transcribe"}) {
        CHECK(transcription_hints(openai(), model, kLanguage, kPhrases).to_json() ==
              Json{{"prompt", "Ksenia, Kubernetes."}, {"language", "de"}});
    }
}

// from tests/test_personalize_s4_openai.py::TestHintFields::test_nothing_without_hints
TEST_CASE("TestHintFields::test_nothing_without_hints", "[test_personalize_s4_openai]") {
    // None, RecognitionContext() and RecognitionContext(language="auto").
    for (const char* language : {"", "auto"}) {
        CHECK(transcription_hints(openai(), "gpt-transcribe", language, {}).empty());
        CHECK(transcription_hints(openai(), "whisper-1", language, {}).empty());
    }
}

// from tests/test_personalize_s4_openai.py::TestHintFields::test_keywords_are_capped
TEST_CASE("TestHintFields::test_keywords_are_capped", "[test_personalize_s4_openai]") {
    std::vector<std::string> phrases;
    for (int i = 0; i < 80; ++i) {
        phrases.push_back("w" + std::to_string(i));
    }
    phrases.push_back(std::string(70, 'x'));
    const auto hints = transcription_hints(openai(), "gpt-transcribe", "", phrases);
    REQUIRE(hints.keywords);
    CHECK(hints.keywords->size() == 50);
}

// ---- TestRequests ----

// from tests/test_personalize_s4_openai.py::TestRequests::test_hints_go_with_the_request
TEST_CASE("TestRequests::test_hints_go_with_the_request", "[test_personalize_s4_openai]") {
    const std::string model = GENERATE("gpt-transcribe", "whisper-1");
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    auto& backend = requests_backend(rig, model);
    CHECK(backend.supports_recognition());
    CHECK(backend.recognition_support() == "model");
    backend.decode(file_job(rig.chunk_files(1).front(), kLanguage, kPhrases));
    const auto sent = rig.http.multipart_parts();
    REQUIRE(sent.size() == 1);
    CHECK(form_of(sent[0]).hints() ==
          wire_fields(transcription_hints(openai(), model, kLanguage, kPhrases)));
}

// from tests/test_personalize_s4_openai.py::TestRequests::test_no_context_sends_exactly_what_it_did
TEST_CASE("TestRequests::test_no_context_sends_exactly_what_it_did",
          "[test_personalize_s4_openai]") {
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    auto& backend = requests_backend(rig, "gpt-transcribe");
    const auto file = rig.chunk_files(1).front();
    backend.decode(file_job(file));
    backend.decode(file_job(file, "", {}));
    const auto sent = rig.http.multipart_parts();
    REQUIRE(sent.size() == 2);
    for (const auto& parts : sent) {
        CHECK(form_of(parts).order == std::vector<std::string>{"model", "response_format", "file"});
    }
}

// from
// tests/test_personalize_s4_openai.py::TestRequests::test_a_rejected_request_is_retried_once_without_hints
TEST_CASE("TestRequests::test_a_rejected_request_is_retried_once_without_hints",
          "[test_personalize_s4_openai]") {
    const int status = GENERATE(400, 422);
    CloudRig rig;
    rig.http.set_handler([status](const http::Request& request) {
        return form_of(request).hints().empty() ? echo_file_name(request) : rejected(status);
    });
    auto sink = std::make_shared<WarningSink>();
    log::add_sink(sink);
    auto& backend = requests_backend(rig, "gpt-transcribe");
    const auto file = rig.chunk_files(1).front();
    const auto text = backend.decode(file_job(file, kLanguage, kPhrases)).transcript.text;
    log::flush(std::chrono::milliseconds(2'000));
    log::remove_sink(sink);
    CHECK(text == "text for part0.wav");
    std::vector<bool> hinted;
    for (const auto& parts : rig.http.multipart_parts()) {
        hinted.push_back(!form_of(parts).hints().empty());
    }
    CHECK(hinted == std::vector<bool>{true, false});
    CHECK(sink->text().find("2 keywords") != std::string::npos);
    CHECK(sink->text().find("Ksenia") == std::string::npos);
    CHECK(sink->text().find("Kubernetes") == std::string::npos);
}

// from tests/test_personalize_s4_openai.py::TestRequests::test_other_failures_are_not_retried
TEST_CASE("TestRequests::test_other_failures_are_not_retried", "[test_personalize_s4_openai]") {
    const int failure = GENERATE(401, 429, 500, 0);
    CAPTURE(failure);
    CloudRig rig;
    rig.http.set_handler([failure](const http::Request&) {
        if (failure == 0) {
            // RuntimeError("offline"): the request never reached a server.
            return http::ScriptedResponse{.error = http::Error{http::ErrorKind::Connect, "x"}};
        }
        return rejected(failure);
    });
    auto& backend = requests_backend(rig, "gpt-transcribe");
    CHECK_THROWS_AS(backend.decode(file_job(rig.chunk_files(1).front(), kLanguage, kPhrases)),
                    SpeechApiError);
    CHECK(rig.http.requests().size() == 1);
}

// from
// tests/test_personalize_s4_openai.py::TestRequests::test_a_rejection_without_hints_is_not_retried
TEST_CASE("TestRequests::test_a_rejection_without_hints_is_not_retried",
          "[test_personalize_s4_openai]") {
    CloudRig rig;
    rig.http.set_handler([](const http::Request&) { return rejected(400); });
    auto& backend = requests_backend(rig, "gpt-transcribe");
    CHECK_THROWS_AS(backend.decode(file_job(rig.chunk_files(1).front())), SpeechApiError);
    CHECK(rig.http.requests().size() == 1);
}

// from
// tests/test_personalize_s4_openai.py::TestRequests::test_the_retired_model_fallback_gets_its_own_hints
TEST_CASE("TestRequests::test_the_retired_model_fallback_gets_its_own_hints",
          "[test_personalize_s4_openai]") {
    CloudRig rig;
    rig.http.set_handler([](const http::Request& request) {
        return form_of(request).field("model") == "whisper-1" ? rejected(404, "model_not_found")
                                                              : echo_file_name(request);
    });
    auto& backend = requests_backend(rig, "whisper-1");
    backend.decode(file_job(rig.chunk_files(1).front(), kLanguage, kPhrases));
    const auto sent = rig.http.multipart_parts();
    REQUIRE(sent.size() == 2);
    CHECK(form_of(sent[0]).field("model") == "whisper-1");
    CHECK(form_of(sent[1]).field("model") == kDefaultApiModel);
    CHECK(form_of(sent[0]).hints() ==
          Fields{{"prompt", {"Ksenia, Kubernetes."}}, {"language", {"de"}}});
    CHECK(form_of(sent[1]).hints() ==
          Fields{{"keywords[]", {"Ksenia", "Kubernetes"}}, {"languages[]", {"de"}}});
}

// from tests/test_personalize_s4_openai.py::TestRequests::test_every_chunk_carries_the_hints
TEST_CASE("TestRequests::test_every_chunk_carries_the_hints", "[test_personalize_s4_openai]") {
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    const auto chunks = rig.chunk_files();
    auto record = rig.stub_splits(chunks);
    auto& backend = rig.make("gpt-transcribe");
    backend.decode(file_job(record->large_file, kLanguage, kPhrases));
    const auto sent = rig.http.multipart_parts();
    REQUIRE(sent.size() == chunks.size());
    for (const auto& parts : sent) {
        CHECK(form_of(parts).hints() ==
              wire_fields(transcription_hints(openai(), "gpt-transcribe", kLanguage, kPhrases)));
    }
}

// from tests/test_personalize_s4_openai.py::test_sdk_sends_the_hint_fields_without_network
TEST_CASE("test_sdk_sends_the_hint_fields_without_network", "[test_personalize_s4_openai]") {
    // Adapted: a real HTTP client to a loopback server stands for the SDK's mock transport.
    const std::string model = GENERATE("gpt-transcribe", "whisper-1");
    CAPTURE(model);
    LoopbackServer server([&model](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(
            model == "gpt-transcribe"
                ? LoopbackServer::response(200, "OK", {{"Content-Type", "application/json"}},
                                           R"({"text": "hello"})")
                : LoopbackServer::response(200, "OK", {{"Content-Type", "text/plain"}}, "hello"));
    });
    CloudRig rig;
    rig.env.set("OPENAI_BASE_URL", server.url("/v1"));
    const auto client = http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    rig.stub_splits({});
    auto& backend = rig.make(model, true, client.get());
    CHECK(
        backend.decode(file_job(rig.chunk_files(1).front(), kLanguage, kPhrases)).transcript.text ==
        "hello");
    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    const std::string& body = requests[0].body;
    const auto count = [&body](const std::string& needle) {
        std::size_t found = 0;
        for (auto at = body.find(needle); at != std::string::npos; at = body.find(needle, at + 1)) {
            ++found;
        }
        return found;
    };
    if (model == "gpt-transcribe") {
        CHECK(count("\r\n\r\nKsenia\r\n") == 1);
        CHECK(count("\r\n\r\nKubernetes\r\n") == 1);
        CHECK(count("name=\"keywords") == 2);
        CHECK(count("name=\"languages") == 1);
        CHECK(count("name=\"prompt\"") == 0);
    } else {
        CHECK(count("name=\"prompt\"\r\n\r\nKsenia, Kubernetes.\r\n") == 1);
        CHECK(count("name=\"language\"\r\n\r\nde\r\n") == 1);
        CHECK(count("name=\"keywords") == 0);
    }
}

// from tests/test_personalize_s4_openai.py::test_sdk_rejection_is_retried_without_hints
TEST_CASE("test_sdk_rejection_is_retried_without_hints", "[test_personalize_s4_openai]") {
    LoopbackServer server([](const LoopbackServer::Request& request, LoopbackServer::Reply& reply) {
        reply.write(request.body.find("name=\"keywords") != std::string::npos
                        ? LoopbackServer::response(400, "Bad Request",
                                                   {{"Content-Type", "application/json"}},
                                                   cloud_reply("openai_bad_keywords.json"))
                        : LoopbackServer::response(200, "OK",
                                                   {{"Content-Type", "application/json"}},
                                                   R"({"text": "plain"})"));
    });
    CloudRig rig;
    rig.env.set("OPENAI_BASE_URL", server.url("/v1"));
    const auto client = http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    rig.stub_splits({});
    auto& backend = rig.make("gpt-transcribe", true, client.get());
    CHECK(
        backend.decode(file_job(rig.chunk_files(1).front(), kLanguage, kPhrases)).transcript.text ==
        "plain");
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].body.find("name=\"keywords") == std::string::npos);
}

// ---- golden vectors ----

TEST_CASE("transcription_hints and response_format match Python", "[golden]") {
    const auto endpoint_of = [](const golden::Case& c) {
        return configured_endpoint(c.arg<std::string>(0), c.arg<Json>(1));
    };
    golden::for_each(
        "transcriber.openai_backend", "transcription_hints", [&](const golden::Case& c) {
            const Json context = c.arg<Json>(3);
            std::string language;
            std::vector<std::string> phrases;
            if (!context.is_null()) {
                language = context[0].get<std::string>();
                phrases = context[1].get<std::vector<std::string>>();
            }
            const auto hints =
                transcription_hints(endpoint_of(c), c.arg<std::string>(2), language, phrases);
            CHECK(golden::equal(c.result<Json>(), hints.to_json()));
        });
    golden::for_each("transcriber.openai_backend", "response_format", [&](const golden::Case& c) {
        CHECK(response_format(endpoint_of(c), c.arg<std::string>(2)) == c.result<std::string>());
    });
}
