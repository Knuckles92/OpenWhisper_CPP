// services/local_text_servers.py: local inference servers' model discovery and detection. Each
// fake answers with the fields its upstream server documents (tests/fixtures/llm/servers/).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "../http/loopback_server.h"
#include "base/executor.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/catalog_client.h"
#include "llm/local_servers.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm_test_support.h"
#include "pyport/json.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::LoopbackServer;

namespace {

using Models = std::vector<std::string>;

// A fixture whose /v1/models list drops `ids` (the Python tests' tables are the fixtures less
// the rows added for the vectors).
Json without_models(Json fixture, const std::vector<std::string>& ids) {
    auto& data = fixture["routes"]["GET /v1/models"]["json"]["data"];
    Json kept = Json::array();
    for (const auto& row : data) {
        if (std::ranges::find(ids, row.value("id", std::string())) == ids.end()) {
            kept.push_back(row);
        }
    }
    data = kept;
    return fixture;
}

Json without_route(Json fixture, const std::string& route) {
    fixture["routes"].erase(route);
    return fixture;
}

// A FakeHttpClient answering from `fixture`, with the discovery state of a fresh process.
struct Server {
    explicit Server(const Json& fixture) {
        forget_server_specs();
        client.set_handler(fixture_server(fixture));
    }
    std::vector<std::string> paths() const {
        std::vector<std::string> out;
        for (const auto& request : client.requests()) {
            out.push_back(url_path(request.url));
        }
        return out;
    }
    http::FakeHttpClient client;
    TestKeys keys;
};

Models list(Server& server, const TextLLMProfile& profile) {
    return list_chat_models(server.client, profile, "", "", {}, &server.keys.sources);
}

// The loopback server answering `fixture`'s routes.
LoopbackServer serving(const Json& fixture) {
    return LoopbackServer([routes = fixture.at("routes")](const LoopbackServer::Request& request,
                                                          LoopbackServer::Reply& reply) {
        const std::string key = request.method + " " + request.target;
        if (!routes.contains(key)) {
            reply.write(LoopbackServer::response(404, "Not Found", {}, "Not Found"));
            return;
        }
        const Json& route = routes.at(key);
        const bool json = route.contains("json");
        reply.write(LoopbackServer::response(
            route.at("status").get<int>(), "OK",
            {{"Content-Type", json ? "application/json" : "text/plain"}},
            json ? py::dumps(route.at("json")) : route.at("text").get<std::string>()));
    });
}

std::shared_ptr<http::Client> direct_client() {
    return http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
}

Json spec_json(const std::optional<TextModelSpec>& spec) {
    return spec ? spec->to_dict() : Json(nullptr);
}

} // namespace

TEST_CASE("vllm reports its context and takes the effort field", "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_vllm_reports_its_context_and_takes_the_effort_field
    Server server(load_fixture("vllm"));
    const auto profile = *get_profile("vllm", Json::object());
    CHECK(list(server, profile) == Models{"Qwen/Qwen3-8B"});
    const auto spec = model_spec(profile, "Qwen/Qwen3-8B");
    CHECK(spec.context_window == 8192);
    CHECK(spec.max_output_tokens == 4096);
    CHECK(spec.reasoning_format == "openai");
    CHECK(spec.thinking_levels == Json{{"off", "none"}});
}

TEST_CASE("llama cpp reports its runtime context", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_llama_cpp_reports_its_runtime_context
    Server server(load_fixture("llama_cpp"));
    const auto profile = *get_profile("llama_cpp", Json::object());
    CHECK(list(server, profile) == Models{"qwen3-4b.gguf"});
    CHECK(model_spec(profile, "qwen3-4b.gguf").context_window == 16384);
}

TEST_CASE("lm studio drops embeddings and reads reasoning options", "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_lm_studio_drops_embeddings_and_reads_reasoning_options
    Server server(without_models(load_fixture("lm_studio"), {"unlisted/model"}));
    const auto profile = *get_profile("lm_studio", Json::object());
    CHECK(list(server, profile) == Models{"google/gemma-3-4b", "qwen/qwen3-4b"});
    const auto qwen = model_spec(profile, "qwen/qwen3-4b");
    CHECK(qwen.context_window == 8192); // what is loaded, not the maximum
    CHECK(qwen.reasoning);
    CHECK(qwen.reasoning_format == "openai");
    CHECK(qwen.thinking_levels ==
          py::loads(R"({"off": null, "low": "low", "medium": "medium", "high": "high"})"));
    const auto gemma = model_spec(profile, "google/gemma-3-4b");
    CHECK(gemma.context_window == 131072);
    CHECK_FALSE(gemma.tools);
    CHECK(gemma.reasoning_format.empty());
}

TEST_CASE("lm studio before 0 4 uses the v0 model list", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_lm_studio_before_0_4_uses_the_v0_model_list
    Server server(load_fixture("lm_studio_v0"));
    const auto profile = *get_profile("lm_studio", Json::object());
    CHECK(list(server, profile) == Models{"google/gemma-3-4b", "qwen/qwen3-4b"});
    CHECK(model_spec(profile, "qwen/qwen3-4b").context_window == 40960);
}

TEST_CASE("localai lists only chat models", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_localai_lists_only_chat_models
    Server server(without_models(load_fixture("localai"), {"piper-tts"}));
    const auto profile = *get_profile("localai", Json::object());
    CHECK(list(server, profile) == Models{"llama-3.2-1b", "qwen3-8b"});
    const auto thinking = model_spec(profile, "qwen3-8b");
    CHECK(thinking.context_window == 32768);
    CHECK(thinking.tools);
    CHECK(thinking.reasoning_format == "openai");
    const auto plain = model_spec(profile, "llama-3.2-1b");
    CHECK(plain.context_window == 32768);
    CHECK_FALSE(plain.tools);
    CHECK(plain.reasoning_format.empty());
}

TEST_CASE("older localai without capabilities falls back to names", "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_older_localai_without_capabilities_falls_back_to_names
    Server server(without_route(without_models(load_fixture("localai"), {"piper-tts"}),
                                "GET /v1/models/capabilities"));
    CHECK(list(server, *get_profile("localai", Json::object())) ==
          Models{"kokoro", "llama-3.2-1b", "qwen3-8b"});
}

TEST_CASE("ollama thinking models take the effort field", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_ollama_thinking_models_take_the_effort_field
    Server server(without_models(load_fixture("ollama"), {"big"}));
    const auto profile = *get_profile("ollama", Json::object());
    CHECK(list(server, profile) == Models{"llama3.2", "qwen3"});
    CHECK(model_spec(profile, "qwen3").thinking_levels == Json{{"off", "none"}});
    const auto plain = model_spec(profile, "llama3.2");
    // No num_ctx: Ollama allocates 4096 tokens unless the GPU is very large.
    CHECK(plain.context_window == 4096);
    CHECK(plain.reasoning_format.empty());
}

TEST_CASE("custom localhost endpoint is identified and matches the preset",
          "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_custom_localhost_endpoint_is_identified_and_matches_the_preset
    Server server(without_models(load_fixture("lm_studio"), {"unlisted/model"}));
    Json settings = Json::object();
    const auto custom = upsert_custom_profile(settings, "Studio", "http://localhost:1234/v1");
    CHECK(list(server, custom) == Models{"google/gemma-3-4b", "qwen/qwen3-4b"});
    const auto preset =
        *get_profile("lm_studio", Json{{"lm_studio_base_url", "http://localhost:1234/v1"}});
    CHECK(model_spec(custom, "qwen/qwen3-4b") == model_spec(preset, "qwen/qwen3-4b"));
    CHECK(model_spec(custom, "qwen/qwen3-4b").context_window == 8192);
    const auto paths = server.paths();
    CHECK(std::ranges::find(paths, "/.well-known/localai.json") != paths.end());
}

TEST_CASE("a preset with a key sends it to discovery", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_a_preset_with_a_key_sends_it_to_discovery
    Server server(load_fixture("vllm"));
    const auto profile = *get_profile("vllm", Json{{"vllm_api_key_env", "VLLM_API_KEY"}});
    server.keys.env.set("VLLM_API_KEY", "sk-local");
    list(server, profile);
    CHECK(http::find_header(server.client.requests()[0].headers, "Authorization") ==
          "Bearer sk-local");
    Server missing(load_fixture("vllm"));
    try {
        list(missing, profile);
        FAIL("expected the missing key to be reported");
    } catch (const CatalogError& error) {
        CHECK(error.type_name() == "RuntimeError");
        CHECK(std::string(error.what()) == "No API key found for vLLM (set VLLM_API_KEY)");
    }
}

TEST_CASE("identify names the server that answers", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_identify_names_the_server_that_answers
    const std::vector<std::pair<std::string, std::optional<std::string>>> cases{
        {"vllm", "vllm"},       {"llama_cpp", "llama_cpp"}, {"lm_studio", "lm_studio"},
        {"localai", "localai"}, {"ollama", "ollama"},       {"empty", std::nullopt},
    };
    for (const auto& [name, expected] : cases) {
        INFO(name);
        Json fixture = load_fixture(name);
        if (name != "vllm") {
            fixture = without_route(fixture, "GET /v1/models");
        }
        Server server(fixture);
        CHECK(identify_server(server.client, "http://127.0.0.1:8080/v1") == expected);
    }
}

TEST_CASE("llama cpp with a key is still identified", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_llama_cpp_with_a_key_is_still_identified
    Server server(load_fixture("llama_cpp_keyed"));
    CHECK(identify_server(server.client, "http://127.0.0.1:8080/v1") == "llama_cpp");
}

TEST_CASE("detection probes only this computer", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_detection_probes_only_this_computer
    auto server = serving(load_fixture("localai"));
    const auto client = direct_client();
    ThreadPool pool("fv-llm-test", 2);
    const std::string url = server.url("/v1");
    // 192.0.2.5 is TEST-NET-1: a remote address that is never probed.
    const auto found =
        detect_running_servers(*client, {url, "http://192.0.2.5:8080/v1", url}, pool);
    CHECK(found == std::vector<std::pair<std::string, std::string>>{{url, "localai"}});
    for (const auto& request : server.requests()) {
        CHECK(request.header("Host").value_or("").starts_with("127.0.0.1"));
    }
    CHECK(server.requests().size() == 1);
}

TEST_CASE("nothing listening is not running", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_nothing_listening_is_not_running
    std::string url;
    {
        auto server = serving(load_fixture("empty"));
        url = server.url("/v1");
    }
    const auto client = direct_client();
    ThreadPool pool("fv-llm-test", 2);
    CHECK(detect_running_servers(*client, {url}, pool).empty());
}

TEST_CASE("detection from a task of its own executor probes in place", "[local_servers]") {
    // Posting the probes to a one-wide strand from its own task and waiting would wait forever.
    Server server(load_fixture("localai"));
    ThreadPool pool("fv-llm-test", 2);
    Strand strand(pool, "fv-llm-probe", 1, ShutdownMode::CancelQueued);
    const std::string url = "http://127.0.0.1:8080/v1";
    std::promise<std::vector<std::pair<std::string, std::string>>> result;
    auto found = result.get_future();
    strand.post(
        [&] { result.set_value(detect_running_servers(server.client, {url, url}, strand)); });
    REQUIRE(found.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    CHECK(found.get() == std::vector<std::pair<std::string, std::string>>{{url, "localai"}});
}

TEST_CASE("detection on an executor that was shut down finds nothing and returns",
          "[local_servers]") {
    Server server(load_fixture("localai"));
    ThreadPool pool("fv-llm-test", 1);
    pool.shutdown(std::chrono::milliseconds(0));
    // The pool drops the posted probes unrun; the wait ends with them.
    CHECK(detect_running_servers(server.client, {"http://127.0.0.1:8080/v1"}, pool).empty());
    CHECK(server.client.requests().empty());
}

TEST_CASE("a custom endpoint's key reaches its identification probes", "[local_servers]") {
    // OpenWhisper's discovery client carries the key on every request (local_text_servers._http),
    // so a server started with a key is identified with it.
    Server server(without_models(load_fixture("lm_studio"), {"unlisted/model"}));
    CHECK(discover_chat_models(server.client, "http://127.0.0.1:1234/v1", std::nullopt,
                               "sk-local") == Models{"google/gemma-3-4b", "qwen/qwen3-4b"});
    const auto requests = server.client.requests();
    REQUIRE(requests.size() > 2);
    for (const auto& request : requests) {
        INFO(request.url);
        CHECK(http::find_header(request.headers, "Authorization") == "Bearer sk-local");
    }
    // Detection asks with no key.
    Server detected(load_fixture("lm_studio"));
    CHECK(identify_server(detected.client, "http://127.0.0.1:1234/v1") == "lm_studio");
    for (const auto& request : detected.client.requests()) {
        CHECK_FALSE(http::find_header(request.headers, "Authorization"));
    }
}

TEST_CASE("learning asks a server the user runs once and leaves hosted endpoints alone",
          "[local_servers]") {
    Server server(load_fixture("ollama"));
    const auto ollama = *get_profile("ollama", Json::object());
    learn_server_models(server.client, ollama, "", &server.keys.sources);
    learn_server_models(server.client, ollama, "", &server.keys.sources);
    const auto paths = server.paths();
    CHECK(std::ranges::count(paths, "/v1/models") == 1);
    CHECK(model_spec(ollama, "qwen3").context_window == 16384);
    // A meeting snapshot records what the server reported.
    const auto saved =
        snapshot_from_profile(ollama, "qwen3", server.client, server.keys.sources).model_metadata;
    CHECK(saved->at("context_window") == 16384);
    CHECK(saved->at("thinking_levels") == Json{{"off", "none"}});

    Server hosted(load_fixture("empty"));
    learn_server_models(hosted.client, *get_profile("groq", Json::object()), "key", nullptr);
    Json settings = Json::object();
    learn_server_models(hosted.client,
                        upsert_custom_profile(settings, "Gateway", "https://gateway.test/v1"),
                        "key", nullptr);
    CHECK(hosted.client.requests().empty());
}

TEST_CASE("describers fail as Python does on answers of the wrong shape", "[local_servers]") {
    auto type_of = [](const std::function<void()>& call) -> std::string {
        try {
            call();
        } catch (const CatalogError& error) {
            return std::string(error.type_name());
        }
        return "";
    };
    CHECK(type_of([] { describe_ollama_model(Json::array()); }) == "AttributeError");
    CHECK(type_of([] { describe_ollama_model(Json{{"capabilities", 5}}); }) == "TypeError");
    CHECK(type_of([] {
              describe_ollama_model(Json{{"capabilities", {"completion"}}, {"parameters", 5}});
          }) == "TypeError");
    // A capabilities string is searched as Python's `in` searches a string.
    CHECK(describe_ollama_model(Json{{"capabilities", "completion,thinking"}})->reasoning);
    CHECK(type_of([] {
              describe_lm_studio({"a"}, Json{{"models", Json::array({{{"key", Json::array()}}})}},
                                 false);
          }) == "TypeError");
    CHECK(type_of([] { describe_localai({"a"}, Json::array()); }) == "AttributeError");
    // set() of an allowed_options string is its characters: none is an effort level.
    const auto specs =
        describe_lm_studio({"m"}, py::loads(R"({"models": [{"key": "m", "type": "llm",
                                   "capabilities": {"reasoning": {"allowed_options": "low"}}}]})"),
                           false);
    CHECK(specs.at("m")->reasoning);
    CHECK_FALSE(specs.at("m")->thinking_levels);
}

TEST_CASE("discovery stops on a cancel", "[local_servers]") {
    Server server(load_fixture("vllm"));
    CancelSource source;
    source.cancel();
    try {
        discover_chat_models(server.client, "http://127.0.0.1:8000/v1", "vllm", std::nullopt,
                             std::chrono::seconds(5), source.token());
        FAIL("expected a cancel");
    } catch (const CatalogError& error) {
        CHECK(error.type_name() == "Cancelled");
    }
}

TEST_CASE("local servers replay services.local_text_servers", "[golden]") {
    const std::string module = "services.local_text_servers";
    golden::for_each(module, "discover_chat_models", [](const golden::Case& c) {
        Server server(load_fixture(c.arg<std::string>(0)));
        const auto base_url = c.arg<std::string>(1);
        const auto preset = c.arg<std::optional<std::string>>(2);
        const auto key = c.arg<std::optional<std::string>>(3);
        Json actual = Json::object();
        try {
            const auto models = discover_chat_models(
                server.client, base_url,
                preset ? std::optional<std::string_view>(*preset) : std::nullopt,
                key ? std::optional<std::string_view>(*key) : std::nullopt);
            actual["models"] = models;
            // A preset at exactly that URL reads what was remembered for it.
            TextLLMProfile reader;
            reader.id = "vllm";
            reader.name = "vLLM";
            reader.kind = "vllm";
            reader.base_url = base_url;
            Json specs = Json::object();
            for (const auto& model : models) {
                specs[model] = model_spec(reader, model).to_dict();
            }
            actual["specs"] = specs;
        } catch (const CatalogError& error) {
            actual["raises"] = std::string(error.type_name());
            actual["message"] = error.what();
        }
        std::vector<std::tuple<std::string, std::string, bool>> seen;
        for (const auto& request : server.client.requests()) {
            const auto auth = http::find_header(request.headers, "Authorization");
            seen.emplace_back(request.method, url_path(request.url),
                              auth.has_value() && auth->starts_with("Bearer "));
        }
        std::ranges::sort(seen);
        const auto repeats = std::ranges::unique(seen);
        seen.erase(repeats.begin(), repeats.end());
        Json requests = Json::array();
        for (const auto& [method, path, bearer] : seen) {
            requests.push_back(Json::array({method, path, bearer}));
        }
        actual["requests"] = requests;
        Json expected = c.result<Json>();
        if (expected.contains("specs")) {
            // The vector lists every remembered spec; a model left out of the list is not one.
            CHECK(expected["specs"].size() == expected["models"].size());
        }
        CHECK(golden::equal(expected, actual));
    });
    golden::for_each(module, "identify_server", [](const golden::Case& c) {
        Server server(load_fixture(c.arg<std::string>(0)));
        const auto found = identify_server(server.client, c.arg<std::string>(1));
        Json requests = Json::array();
        for (const auto& request : server.client.requests()) {
            requests.push_back(
                Json::array({request.method, url_path(request.url),
                             http::find_header(request.headers, "Authorization").has_value()}));
        }
        const Json actual{{"server", found ? Json(*found) : Json(nullptr)}, {"requests", requests}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "server_root", [](const golden::Case& c) {
        CHECK(server_root(c.arg<std::string>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "_spec", [](const golden::Case& c) {
        const auto effort = c.arg<Json>(3);
        const auto spec =
            server_spec(c.arg<std::int64_t>(0), c.arg<bool>(1), c.arg<bool>(2),
                        effort.is_null() ? std::nullopt : std::optional<Json>(effort));
        CHECK(golden::equal(c.result<Json>(), spec.to_dict()));
    });
    golden::for_each(module, "_by_name", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), spec_json(spec_by_name(c.arg<std::string>(0)))));
    });
}
