// CloudSpeechBackend (transcriber/openai_backend.py): request fields, chunked uploads 3 at a
// time in chunk order, cancel, the retired-model fallback, and the multipart request on the
// wire. Uploads go to FakeHttpClient or to a loopback server; nothing reaches the internet.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../http/loopback_server.h"
#include "audio/wav_writer.h"
#include "base/testing/manual_executor.h"
#include "cloud_test_support.h"
#include "http/client.h"
#include "speech/api_models.h"

using namespace fv;
using namespace fv::speech;
using namespace fv::speech::test;
using fv::testing::LoopbackServer;
using namespace std::chrono_literals;

namespace {

std::string error_of(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

std::vector<std::string> file_names(const http::FakeHttpClient& http) {
    std::vector<std::string> names;
    for (const auto& parts : http.multipart_parts()) {
        names.push_back(form_of(parts).filename);
    }
    return names;
}

std::string joined_texts(int count) {
    std::string out;
    for (int index = 0; index < count; ++index) {
        out += (out.empty() ? "" : " ") + std::string("text for part") + std::to_string(index) +
               ".wav";
    }
    return out;
}

} // namespace

// ---- TestChunkedTranscription ----

// from tests/test_openai_backend.py::TestChunkedTranscription::test_transcripts_keep_chunk_order
TEST_CASE("TestChunkedTranscription::test_transcripts_keep_chunk_order", "[test_openai_backend]") {
    // The first chunk is made the slowest so it finishes last; the text still reads in order.
    CloudRig rig;
    rig.http.set_handler([](const http::Request& request) {
        auto reply = echo_file_name(request);
        if (form_of(request).filename == "part0.wav") {
            reply.on_chunk = [] {
                std::this_thread::sleep_for(150ms);
            };
        }
        return reply;
    });
    auto record = rig.stub_splits(rig.chunk_files());
    auto& backend = rig.make();
    const auto result = backend.decode(file_job(record->large_file));
    CHECK(result.transcript.text == joined_texts(6));
}

// from tests/test_openai_backend.py::TestChunkedTranscription::test_uploads_actually_overlap
TEST_CASE("TestChunkedTranscription::test_uploads_actually_overlap", "[test_openai_backend]") {
    CloudRig rig;
    InFlight in_flight;
    rig.http.set_handler([&in_flight](const http::Request& request) {
        auto reply = echo_file_name(request);
        reply.on_chunk = [&in_flight] {
            in_flight.enter();
            std::this_thread::sleep_for(50ms);
            in_flight.leave();
        };
        return reply;
    });
    auto record = rig.stub_splits(rig.chunk_files());
    rig.make().decode(file_job(record->large_file));
    CHECK(in_flight.peak > 1);
    CHECK(in_flight.peak <= kChunkUploadConcurrency);
}

// from tests/test_openai_backend.py::TestChunkedTranscription::test_concurrency_is_capped
TEST_CASE("TestChunkedTranscription::test_concurrency_is_capped", "[test_openai_backend]") {
    // No retry path here, so a burst of uploads risks a 429.
    CloudRig rig;
    InFlight in_flight;
    rig.http.set_handler([&in_flight](const http::Request& request) {
        auto reply = echo_file_name(request);
        reply.on_chunk = [&in_flight] {
            in_flight.enter();
            std::this_thread::sleep_for(50ms);
            in_flight.leave();
        };
        return reply;
    });
    auto chunks = rig.chunk_files();
    std::vector<std::filesystem::path> many;
    for (int round = 0; round < 4; ++round) {
        many.insert(many.end(), chunks.begin(), chunks.end());
    }
    auto record = rig.stub_splits(many);
    rig.make().decode(file_job(record->large_file));
    CHECK(in_flight.peak <= kChunkUploadConcurrency);
    CHECK(rig.http.requests().size() == 24);
}

// from tests/test_openai_backend.py::TestChunkedTranscription::test_a_single_chunk_skips_the_pool
TEST_CASE("TestChunkedTranscription::test_a_single_chunk_skips_the_pool", "[test_openai_backend]") {
    // Adapted: Python uploads a lone chunk on the job's own thread. Here even one upload runs on
    // the upload strand (never on fv-speech), and alone.
    CloudRig rig;
    InFlight in_flight;
    std::set<std::thread::id> threads;
    std::mutex threads_mutex;
    rig.http.set_handler([&](const http::Request& request) {
        auto reply = echo_file_name(request);
        reply.on_chunk = [&] {
            in_flight.enter();
            {
                std::lock_guard lock(threads_mutex);
                threads.insert(std::this_thread::get_id());
            }
            in_flight.leave();
        };
        return reply;
    });
    auto record = rig.stub_splits({rig.chunk_files().front()});
    const auto result = rig.make().decode(file_job(record->large_file));
    CHECK(result.transcript.text == "text for part0.wav");
    CHECK(in_flight.peak == 1);
    CHECK_FALSE(threads.contains(std::this_thread::get_id()));
}

// from
// tests/test_openai_backend.py::TestChunkedTranscription::test_cancel_before_start_stops_the_run
TEST_CASE("TestChunkedTranscription::test_cancel_before_start_stops_the_run",
          "[test_openai_backend]") {
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    auto record = rig.stub_splits(rig.chunk_files());
    auto& backend = rig.make();
    CancelSource cancel;
    cancel.cancel();
    auto job = file_job(record->large_file);
    job.cancel = cancel.token();
    CHECK(error_of([&] { backend.decode(job); }).find("canceled") != std::string::npos);
    CHECK(rig.http.requests().empty());
    CHECK(record->split_calls().empty());
    // Cancelled before any chunk existed.
    CHECK(record->cleanup_calls() == 0);
}

// from tests/test_openai_backend.py::TestChunkedTranscription::
// test_cancel_while_splitting_uploads_nothing
TEST_CASE("TestChunkedTranscription::test_cancel_while_splitting_uploads_nothing",
          "[test_openai_backend]") {
    // The split is the slow local step; a cancel during it saves every upload.
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    auto record = rig.stub_splits(rig.chunk_files());
    CancelSource cancel;
    record->during_split = [&cancel] {
        cancel.cancel();
    };
    auto& backend = rig.make();
    auto job = file_job(record->large_file);
    job.cancel = cancel.token();
    CHECK(error_of([&] { backend.decode(job); }).find("canceled") != std::string::npos);
    CHECK(rig.http.requests().empty());
    CHECK(record->cleanup_calls() == 1);
}

// from
// tests/test_openai_backend.py::TestChunkedTranscription::test_cancel_mid_flight_stops_later_chunks
TEST_CASE("TestChunkedTranscription::test_cancel_mid_flight_stops_later_chunks",
          "[test_openai_backend]") {
    // Cancel latency is one in-flight upload, not the whole file.
    CloudRig rig;
    CancelSource cancel;
    std::atomic<int> answered{0};
    rig.http.set_handler([&](const http::Request& request) {
        auto reply = echo_file_name(request);
        reply.on_chunk = [&] {
            std::this_thread::sleep_for(20ms);
            if (++answered >= 2) {
                cancel.cancel();
            }
        };
        return reply;
    });
    auto record = rig.stub_splits(rig.chunk_files());
    auto& backend = rig.make();
    auto job = file_job(record->large_file);
    job.cancel = cancel.token();
    CHECK(error_of([&] { backend.decode(job); }).find("canceled") != std::string::npos);
    CHECK(rig.http.requests().size() < 6);
}

// from tests/test_openai_backend.py::TestChunkedTranscription::
// test_a_failed_upload_propagates_and_temp_files_go
TEST_CASE("TestChunkedTranscription::test_a_failed_upload_propagates_and_temp_files_go",
          "[test_openai_backend]") {
    CloudRig rig;
    rig.http.set_handler([](const http::Request& request) {
        if (form_of(request).filename == "part3.wav") {
            http::ScriptedResponse rejected;
            rejected.status = 500;
            rejected.body = "upload rejected";
            return rejected;
        }
        return echo_file_name(request);
    });
    auto record = rig.stub_splits(rig.chunk_files());
    auto& backend = rig.make();
    CHECK(error_of([&] { backend.decode(file_job(record->large_file)); }).find("upload rejected") !=
          std::string::npos);
    CHECK(record->cleanup_calls() == 1);
}

// from
// tests/test_openai_backend.py::TestChunkedTranscription::test_is_transcribing_is_cleared_on_failure
TEST_CASE("TestChunkedTranscription::test_is_transcribing_is_cleared_on_failure",
          "[test_openai_backend]") {
    // Adapted: the backend keeps no busy flag (the speech service does). A failed decode leaves
    // nothing behind that stops the next one.
    CloudRig rig;
    std::atomic<bool> fail{true};
    rig.http.set_handler([&fail](const http::Request& request) {
        if (fail && form_of(request).filename == "part0.wav") {
            http::ScriptedResponse rejected;
            rejected.status = 500;
            rejected.body = "upload rejected";
            return rejected;
        }
        return echo_file_name(request);
    });
    auto record = rig.stub_splits(rig.chunk_files());
    auto& backend = rig.make();
    CHECK_THROWS_AS(backend.decode(file_job(record->large_file)), SpeechApiError);
    fail = false;
    CHECK(backend.decode(file_job(record->large_file)).transcript.text == joined_texts(6));
}

// from tests/test_openai_backend.py::TestChunkedTranscription::
// test_unavailable_backend_refuses_before_splitting
TEST_CASE("TestChunkedTranscription::test_unavailable_backend_refuses_before_splitting",
          "[test_openai_backend]") {
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    auto record = rig.stub_splits(rig.chunk_files());
    auto& backend = rig.make();
    backend.warm();
    rig.credentials.remove("OPENAI_API_KEY");
    CHECK(error_of([&] { backend.decode(file_job(record->large_file)); }) ==
          "OpenAI transcription is not available (no API key or client initialization failed)");
    CHECK(record->split_calls().empty());
}

// from
// tests/test_openai_backend.py::TestChunkedTranscription::test_a_file_under_the_limit_is_one_upload
TEST_CASE("TestChunkedTranscription::test_a_file_under_the_limit_is_one_upload",
          "[test_openai_backend]") {
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    const auto chunks = rig.chunk_files();
    auto record = rig.stub_splits(chunks);
    auto& backend = rig.make();
    CHECK_FALSE(backend.large_file_size_mb(chunks[0]).has_value());
    CHECK(backend.decode(file_job(chunks[0])).transcript.text == "text for part0.wav");
    CHECK(record->split_calls().empty());
    CHECK(file_names(rig.http) == std::vector<std::string>{"part0.wav"});
}

// from tests/test_openai_backend.py::TestChunkedTranscription::
// test_a_large_file_reports_each_step_and_cleans_up
TEST_CASE("TestChunkedTranscription::test_a_large_file_reports_each_step_and_cleans_up",
          "[test_openai_backend]") {
    // The runtime turns these into the status line and the overlay stage.
    CloudRig rig;
    rig.http.set_handler(echo_file_name);
    auto record = rig.stub_splits(rig.chunk_files(), 42.0);
    auto& backend = rig.make();
    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> progress;
    std::vector<double> detected;
    ManualExecutor inline_executor;
    const auto subscription = backend.large_file_detected.connect(
        inline_executor, [&](double megabytes) { detected.push_back(megabytes); });
    auto job = file_job(record->large_file);
    job.on_progress = [&](const std::string& message, bool transcribing) {
        std::lock_guard lock(mutex);
        progress.emplace_back(message, transcribing);
    };
    CHECK(backend.large_file_size_mb(record->large_file) == 42.0);
    backend.decode(job);
    inline_executor.run_all();
    CHECK(record->split_calls() == std::vector<std::filesystem::path>{record->large_file});
    CHECK(progress == std::vector<std::pair<std::string, bool>>{
                          {"Loading audio file...", false}, {"Transcribing 6 chunks...", true}});
    CHECK(detected == std::vector<double>{42.0});
    CHECK(record->cleanup_calls() == 1);
}

// from tests/test_openai_backend.py::test_selected_api_model_and_response_format
TEST_CASE("test_selected_api_model_and_response_format", "[test_openai_backend]") {
    const std::string model =
        GENERATE("gpt-transcribe", "gpt-4o-transcribe", "gpt-4o-mini-transcribe", "whisper-1");
    const bool chunked = GENERATE(false, true);
    CAPTURE(model, chunked);
    CloudRig rig;
    rig.settings["api_transcription_model"] = model;
    rig.http.set_handler([&model](const http::Request&) {
        return model == "gpt-transcribe" ? json_reply(200, R"({"text": "  transcript  "})")
                                         : text_reply("  transcript  ");
    });
    const auto chunks = rig.chunk_files();
    auto record = rig.stub_splits({chunks[0], chunks[1]});
    auto& backend = rig.make("api");
    const auto result = backend.decode(file_job(chunked ? record->large_file : chunks[0]));
    CHECK(result.transcript.text == (chunked ? "transcript transcript" : "transcript"));
    const auto sent = rig.http.multipart_parts();
    REQUIRE(sent.size() == (chunked ? 2u : 1u));
    for (const auto& parts : sent) {
        const SentForm form = form_of(parts);
        CHECK(form.field("model") == model);
        CHECK(form.field("response_format") == (model == "gpt-transcribe" ? "json" : "text"));
    }
}

// from tests/test_openai_backend.py::test_chunks_keep_model_when_setting_changes_mid_upload
TEST_CASE("test_chunks_keep_model_when_setting_changes_mid_upload", "[test_openai_backend]") {
    CloudRig rig;
    rig.settings["api_transcription_model"] = "gpt-transcribe";
    std::mutex mutex;
    rig.http.set_handler([&](const http::Request&) {
        auto reply = json_reply(200, R"({"text": "transcript"})");
        reply.on_chunk = [&] {
            std::lock_guard lock(mutex);
            rig.apply([](Json& settings) { settings["api_transcription_model"] = "whisper-1"; });
        };
        return reply;
    });
    auto record = rig.stub_splits(rig.chunk_files());
    rig.make("api").decode(file_job(record->large_file));
    for (const auto& parts : rig.http.multipart_parts()) {
        CHECK(form_of(parts).field("model") == "gpt-transcribe");
    }
}

// from tests/test_openai_backend.py::test_sdk_sends_model_and_parses_response_without_network
TEST_CASE("test_sdk_sends_model_and_parses_response_without_network", "[test_openai_backend]") {
    // Adapted: the request crosses a real HTTP client to a loopback server shaped like OpenAI's
    // (the SDK and its mock transport stand for that in Python).
    const std::string model = GENERATE("gpt-transcribe", "whisper-1");
    CAPTURE(model);
    LoopbackServer server([&model](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(model == "gpt-transcribe"
                        ? LoopbackServer::response(200, "OK",
                                                   {{"Content-Type", "application/json"}},
                                                   cloud_reply("openai_gpt_transcribe.json"))
                        : LoopbackServer::response(200, "OK", {{"Content-Type", "text/plain"}},
                                                   cloud_reply("openai_whisper_text.txt")));
    });
    CloudRig rig;
    rig.env.set("OPENAI_BASE_URL", server.url("/v1"));
    const auto client = http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    auto& backend = rig.make(model, true, client.get());
    const auto chunk = rig.chunk_files(1).front();
    CHECK(backend.decode(file_job(chunk)).transcript.text == "hello");
    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == "POST");
    CHECK(requests[0].target == "/v1/audio/transcriptions");
    CHECK(requests[0].header("Authorization") == "Bearer sk-test");
    CHECK(requests[0].header("Content-Type")->starts_with("multipart/form-data; boundary="));
    const std::string& body = requests[0].body;
    CHECK(body.find("name=\"model\"\r\n\r\n" + model + "\r\n") != std::string::npos);
    const std::string format = model == "gpt-transcribe" ? "json" : "text";
    CHECK(body.find("name=\"response_format\"\r\n\r\n" + format + "\r\n") != std::string::npos);
    CHECK(body.find("name=\"file\"; filename=\"part0.wav\"\r\nContent-Type: audio/wav\r\n\r\n" +
                    read_file(chunk) + "\r\n") != std::string::npos);
    // model first, the file last, as the SDK orders them.
    CHECK(body.find("name=\"model\"") < body.find("name=\"response_format\""));
    CHECK(body.find("name=\"response_format\"") < body.find("name=\"file\""));
}

// ---- TestTranscriptionRetry (tests/test_openai_retirement.py) ----

// from tests/test_openai_retirement.py::TestTranscriptionRetry::
// test_gone_retiring_model_retries_with_gpt_transcribe
TEST_CASE("TestTranscriptionRetry::test_gone_retiring_model_retries_with_gpt_transcribe",
          "[test_openai_retirement]") {
    CloudRig rig;
    std::vector<std::string> models;
    std::mutex mutex;
    rig.http.set_handler([&](const http::Request& request) {
        const SentForm form = form_of(request);
        std::lock_guard lock(mutex);
        models.push_back(form.field("model") == "whisper-1" ? "whisper-1" : "other");
        if (form.field("model") == "whisper-1") {
            return json_reply(404, cloud_reply("openai_model_not_found.json"));
        }
        CHECK(form.field("model") == "gpt-transcribe");
        CHECK(form.field("response_format") == "json");
        return json_reply(200, R"({"text": " hello ", "languages": []})");
    });
    auto& backend = rig.make("whisper-1");
    CHECK(backend.decode(file_job(rig.chunk_files(1).front())).transcript.text == "hello");
    CHECK(models == std::vector<std::string>{"whisper-1", "other"});
}

// from tests/test_openai_retirement.py::TestTranscriptionRetry::test_other_errors_are_not_retried
TEST_CASE("TestTranscriptionRetry::test_other_errors_are_not_retried", "[test_openai_retirement]") {
    CloudRig rig;
    rig.http.set_handler([](const http::Request&) {
        return json_reply(400, cloud_reply("openai_invalid_value.json"));
    });
    auto& backend = rig.make("whisper-1");
    try {
        backend.decode(file_job(rig.chunk_files(1).front()));
        FAIL("the request should have failed");
    } catch (const SpeechApiError& error) {
        CHECK(error.status == 400);
        CHECK(error.code == "invalid_value");
        // str() of the SDK's BadRequestError.
        CHECK(std::string(error.what()) ==
              "Error code: 400 - {'error': {'message': 'bad audio', 'type': "
              "'invalid_request_error', 'code': 'invalid_value'}}");
    }
    CHECK(rig.http.requests().size() == 1);
}

// from
// tests/test_openai_retirement.py::TestTranscriptionRetry::test_gpt_transcribe_itself_is_not_retried
TEST_CASE("TestTranscriptionRetry::test_gpt_transcribe_itself_is_not_retried",
          "[test_openai_retirement]") {
    CloudRig rig;
    rig.http.set_handler([](const http::Request&) {
        return json_reply(
            404,
            R"({"error": {"message": "gone", "type": "invalid_request_error", "code": "model_not_found"}})");
    });
    auto& backend = rig.make("gpt-transcribe");
    CHECK_THROWS_AS(backend.decode(file_job(rig.chunk_files(1).front())), SpeechApiError);
    CHECK(rig.http.requests().size() == 1);
}

// from tests/test_openai_retirement.py::TestTranscriptionRetry::
// test_explicit_retiring_model_type_maps_after_shutdown
TEST_CASE("TestTranscriptionRetry::test_explicit_retiring_model_type_maps_after_shutdown",
          "[test_openai_retirement]") {
    CloudRig rig;
    rig.clock.set_utc(std::chrono::sys_days{std::chrono::year{2027} / 2 / 26} + 12h);
    CHECK(rig.make("whisper-1").target().second == "gpt-transcribe");
    CHECK(rig.make("gpt-4o-transcribe").target().second == "gpt-transcribe");
}

// ---- the backend's own contract ----

TEST_CASE("the api backend's capabilities, readiness and name", "[cloud_backend]") {
    CloudRig rig;
    auto& backend = rig.make("api", false);
    CHECK(backend.id() == "api");
    const Capabilities capabilities = backend.capabilities();
    CHECK_FALSE(capabilities.windows);
    CHECK_FALSE(capabilities.live_preview);
    CHECK(capabilities.accepts_files);
    CHECK(capabilities.max_window_s == 0);
    CHECK(capabilities.split_over_mb == 23.0);
    CHECK(backend.supports_recognition());
    CHECK(backend.prepares_for_transcription());
    CHECK(backend.recognition_support() == "model");

    const Readiness missing = backend.load({});
    CHECK(missing.state == ReadyState::NeedsKey);
    CHECK(missing.message == "The selected API transcription engine needs a valid API key");

    rig.credentials.set("OPENAI_API_KEY", "sk-test");
    const Readiness ready = backend.load({});
    CHECK(ready.state == ReadyState::Ready);
    CHECK(ready.device_info == "OpenAI (gpt-transcribe)");

    rig.apply([](Json& settings) {
        save_server(settings, "speaches", "http://127.0.0.1:8000/v1", "", "tiny");
        save_selection(settings, "speaches");
    });
    CHECK(backend.name() == "Speaches (tiny)");
    CHECK(backend.load({}).state == ReadyState::Ready);
}

TEST_CASE("an upload cancelled in flight is abandoned at once and never retried",
          "[cloud_backend]") {
    LoopbackServer server(
        [](const LoopbackServer::Request&, LoopbackServer::Reply& reply) { reply.wait(30s); });
    CloudRig rig;
    rig.env.set("OPENAI_BASE_URL", server.url("/v1"));
    const auto client = http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
    auto& backend = rig.make("whisper-1", true, client.get());
    CancelSource cancel;
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(300ms);
        cancel.cancel();
    });
    auto job = file_job(rig.chunk_files(1).front());
    job.cancel = cancel.token();
    const auto started = std::chrono::steady_clock::now();
    const std::string error = error_of([&] { backend.decode(job); });
    canceller.join();
    CHECK(error == "Transcription canceled");
    CHECK(std::chrono::steady_clock::now() - started < 10s);
    CHECK(server.requests().size() == 1);
}

TEST_CASE("connection failures and timeouts are worded as the SDK words them", "[cloud_backend]") {
    CloudRig rig;
    auto& backend = rig.make("whisper-1");
    const auto file = rig.chunk_files(1).front();

    rig.http.push(http::ScriptedResponse{.error = http::Error{http::ErrorKind::Dns, "no"}});
    CHECK(error_of([&] { backend.decode(file_job(file)); }) == "Connection error.");

    rig.http.push(
        http::ScriptedResponse{.error = http::Error{http::ErrorKind::Timeout, "timed out"}});
    CHECK(error_of([&] { backend.decode(file_job(file)); }) == "Request timed out.");

    // An error body that is not JSON is the message itself; an empty one names the status.
    http::ScriptedResponse plain;
    plain.status = 502;
    plain.body = "  Bad gateway \n";
    rig.http.push(plain);
    CHECK(error_of([&] { backend.decode(file_job(file)); }) == "Bad gateway");
    http::ScriptedResponse empty;
    empty.status = 503;
    rig.http.push(empty);
    CHECK(error_of([&] { backend.decode(file_job(file)); }) == "Error code: 503");
    CHECK(rig.http.requests().size() == 4);
}

TEST_CASE("a json reply's text is read as the SDK reads a Transcription", "[cloud_backend]") {
    http::Response response;
    response.status = 200;
    response.headers = {{"Content-Type", "application/json; charset=utf-8"}};
    response.body = R"({"text": " hi ", "usage": {"type": "duration", "seconds": 1}})";
    CHECK(parse_transcription_reply(response, "json") == " hi ");
    // Content that is not JSON comes back as it is, unless it parses anyway.
    response.headers = {{"Content-Type", "text/plain"}};
    response.body = "plain words";
    CHECK(parse_transcription_reply(response, "json") == "plain words");
    response.body = R"({"text": "parsed"})";
    CHECK(parse_transcription_reply(response, "json") == "parsed");
    // response_format "text" is the body as sent.
    CHECK(parse_transcription_reply(response, "text") == R"({"text": "parsed"})");
    response.headers = {{"Content-Type", "application/json"}};
    response.body = R"({"duration": 1})";
    CHECK_THROWS_WITH(parse_transcription_reply(response, "json"),
                      "'Transcription' object has no attribute 'text'");
}

TEST_CASE("temporary chunk files are removed however the upload ends", "[cloud_backend]") {
    // A real split of a 6 s WAV into chunks of about 2.6 s, one of which the server rejects.
    CloudRig rig;
    rig.options.split_limits = SplitLimits{0.04, 0.1, 1.0, 0.3, 0.01};
    std::vector<std::filesystem::path> seen;
    std::mutex mutex;
    rig.http.set_handler([&](const http::Request& request) {
        std::lock_guard lock(mutex);
        for (const auto& part : std::get<http::Multipart>(request.body).parts) {
            if (const auto* path = std::get_if<std::filesystem::path>(&part.data)) {
                seen.push_back(*path);
            }
        }
        http::ScriptedResponse rejected;
        rejected.status = seen.size() == 2 ? 400 : 200;
        rejected.body = seen.size() == 2 ? "no" : "fine";
        rejected.headers = {{"Content-Type", "text/plain"}};
        return rejected;
    });
    auto& backend = rig.make("whisper-1");
    const auto wav = rig.root.path() / "long.wav";
    {
        audio::WavWriter writer(wav, audio::WavWriter::Format{8000, 1, 2});
        std::vector<std::int16_t> samples(8000 * 6);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<std::int16_t>((i * 7919) % 20000) - 10000;
        }
        writer.write(samples);
        writer.commit(std::nullopt);
    }
    REQUIRE(backend.large_file_size_mb(wav).has_value());
    CHECK_THROWS_AS(backend.decode(file_job(wav)), SpeechApiError);
    REQUIRE(seen.size() >= 2);
    for (const auto& chunk : seen) {
        CHECK_FALSE(std::filesystem::exists(chunk));
        CHECK_FALSE(std::filesystem::exists(chunk.parent_path()));
    }
}

TEST_CASE("uploads the executor drops end the decode instead of leaving it waiting",
          "[cloud_backend]") {
    // At quit the upload strand drops what it has not started; decode() must not wait for it.
    CloudRig rig;
    auto& backend = rig.make("whisper-1");
    const auto file = rig.chunk_files(1).front();
    rig.uploads.shutdown(0ms);
    auto decoding = std::async(std::launch::async,
                               [&] { return error_of([&] { backend.decode(file_job(file)); }); });
    REQUIRE(decoding.wait_for(10s) == std::future_status::ready);
    CHECK(decoding.get() == "Transcription canceled");
    CHECK(rig.http.requests().empty());
}

TEST_CASE("an audio clip is refused: the api backend uploads files", "[cloud_backend]") {
    CloudRig rig;
    DecodeJob job;
    job.input = AudioClip{};
    CHECK_THROWS_AS(rig.make().decode(job), std::invalid_argument);
}

TEST_CASE("Groq transcribes a fixture through its OpenAI-compatible route",
          "[cloud_backend][platform]") {
    // Spends the owner's Groq credit: runs only when OPENWHISPER_CPP_TEST_GROQ_KEY is set.
    const auto key = testkit::get_env("OPENWHISPER_CPP_TEST_GROQ_KEY");
    if (!key || key->empty()) {
        SKIP("OPENWHISPER_CPP_TEST_GROQ_KEY is not set");
    }
    CloudRig rig;
    rig.credentials.set("GROQ_API_KEY", *key);
    rig.apply([](Json& settings) { save_selection(settings, "groq"); });
    const auto client = http::make_curl_client();
    auto& backend = rig.make("api", false, client.get());
    const auto result = backend.decode(
        file_job(testkit::source_root() / "tests" / "fixtures" / "audio" / "tone-44k1-mono.wav"));
    // A tone has no words; the request succeeding is the check.
    CHECK(result.transcript.text.size() < 200);
}
