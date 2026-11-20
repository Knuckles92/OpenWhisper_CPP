// The speaker-embedding model: where it is found, when it may download, how a download is
// verified and how its failures read (the speaker tests of OpenWhisper's tests/test_components.py
// and the services.components_speaker vectors). Downloads go to servers on the loopback interface
// (or a scripted client) and fetch a 64-byte stand-in whose digest the test pins.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../http/loopback_server.h"
#include "base/cancel.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/speaker_model.h"
#include "models/testing/speaker_model_state.h"
#include "models_test_support.h"
#include "settings/store.h"

using namespace fv::models;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::models::test::read_file;
using fv::models::test::TempDir;
using fv::models::test::write_file;
using fv::testing::LoopbackServer;
namespace fs = std::filesystem;
namespace golden = fv::golden;

namespace {

const std::string kModelBytes(64, 'w');

std::string utf8(const fs::path& path) {
    return fv::base_utf8::path_to_utf8(path);
}

std::string env_name(const fv::AppIdentity& app) {
    return std::string(app.env_prefix) + "SPEAKER_MODEL";
}

// A temporary LOCALAPPDATA (so the speaker-model folder is a test folder), a settings file and a
// curl client that never uses a proxy.
struct Rig {
    TempDir temp;
    fv::MapEnvReader env{{"LOCALAPPDATA", utf8(temp.path())},
                         {"USERPROFILE", utf8(temp.path())},
                         {"HOME", utf8(temp.path())},
                         {"XDG_DATA_HOME", utf8(temp.path())}};
    fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore store{temp.path() / "settings.json"};
    std::shared_ptr<fv::http::Client> client = [] {
        fv::http::CurlClientOptions options;
        options.proxy = [](std::string_view) {
            return std::optional<std::string>();
        };
        return fv::http::make_curl_client(options);
    }();

    Rig() { testing::reset_speaker_model_error(); }

    fs::path cache() const { return speaker_model_cache_dir(paths); }
    fs::path destination() const { return cache() / "voxceleb_resnet34_LM.onnx"; }

    // Deps that fetch `bytes`'s stand-in from `url`.
    SpeakerModelDeps deps(const std::string& url, fv::http::Client* other = nullptr) {
        SpeakerModelDeps made(paths, env, store, other != nullptr ? *other : *client);
        made.url = url;
        made.sha256 = fv::sha256_hex(kModelBytes);
        made.size_bytes = static_cast<std::int64_t>(kModelBytes.size());
        return made;
    }
};

LoopbackServer::Handler serve(std::string body) {
    return [body = std::move(body)](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(200, "OK", {}, body));
    };
}

std::string message_of(const std::function<void()>& run) {
    try {
        run();
    } catch (const ComponentError& error) {
        return error.what();
    }
    return "(no ComponentError was thrown)";
}

// A loopback URL on which nothing listens: the port of a server that has stopped.
std::string closed_url() {
    std::string url;
    {
        LoopbackServer gone(serve(kModelBytes));
        url = gone.url("/voxceleb_resnet34_LM.onnx");
    }
    return url;
}

} // namespace

TEST_CASE("test_speaker_model_path_is_none_without_a_local_model", "[test_components]") {
    Rig rig;
    CHECK_FALSE(speaker_model_path(rig.paths, rig.env));
}

TEST_CASE("test_speaker_model_path_honors_env_file", "[test_components]") {
    Rig rig;
    const fs::path model = rig.temp.path() / "custom.onnx";
    write_file(model, "onnx");
    write_file(rig.destination(), "onnx");
    rig.env.set(env_name(fv::legacy_identity()), utf8(model));
    // The variable wins over the cache.
    CHECK(speaker_model_path(rig.paths, rig.env) == model);

    // FioriVoce's own variable comes first.
    const fs::path own = rig.temp.path() / "own.ONNX";
    write_file(own, "onnx");
    rig.env.set(env_name(fv::identity()), "  " + utf8(own) + "  ");
    CHECK(speaker_model_path(rig.paths, rig.env) == own);
}

TEST_CASE("test_speaker_model_path_honors_env_directory", "[test_components]") {
    Rig rig;
    const fs::path folder = rig.temp.path() / "speaker";
    const fs::path model = folder / "model.onnx";
    write_file(model, "onnx");
    rig.env.set(env_name(fv::legacy_identity()), utf8(folder));
    CHECK(speaker_model_path(rig.paths, rig.env) == model);
}

TEST_CASE("test_speaker_model_path_uses_cache_dir", "[test_components]") {
    Rig rig;
    CHECK(rig.cache() == rig.temp.path() / "FioriVoce" / "models" / "speaker-id");
    write_file(rig.destination(), "onnx");
    CHECK(speaker_model_path(rig.paths, rig.env) == rig.destination());
}

TEST_CASE("test_speaker_model_path_falls_back_to_source_tree: no source tree is consulted",
          "[test_components]") {
    // OpenWhisper run from source takes <checkout>/models/speaker-id; an installed app has no
    // checkout, so a models/speaker-id folder in the working directory is not a model.
    Rig rig;
    const fs::path checkout = rig.temp.path() / "checkout";
    write_file(checkout / "models" / "speaker-id" / "voxceleb_resnet34_LM.onnx", "onnx");
    const fs::path previous = fs::current_path();
    fs::current_path(checkout);
    const auto found = speaker_model_path(rig.paths, rig.env);
    fs::current_path(previous);
    CHECK_FALSE(found);
}

TEST_CASE("a variable naming a file that is not .onnx, or a folder without one, is skipped",
          "[test_components]") {
    Rig rig;
    const fs::path text = rig.temp.path() / "model.bin";
    write_file(text, "onnx");
    rig.env.set(env_name(fv::identity()), utf8(text));
    CHECK_FALSE(speaker_model_path(rig.paths, rig.env));

    const fs::path empty = rig.temp.path() / "empty";
    fs::create_directories(empty);
    rig.env.set(env_name(fv::legacy_identity()), utf8(empty));
    CHECK_FALSE(speaker_model_path(rig.paths, rig.env));

    // Both skipped, the cache still counts.
    write_file(rig.destination(), "onnx");
    CHECK(speaker_model_path(rig.paths, rig.env) == rig.destination());
}

TEST_CASE("_first_onnx_file takes a folder's sorted files before its subfolders",
          "[test_components]") {
    TempDir temp;
    const fs::path root = temp.path();
    CHECK_FALSE(first_onnx_file(root / "missing"));
    write_file(root / "readme.txt", "x");
    CHECK_FALSE(first_onnx_file(root));
    write_file(root / "a" / "deep.onnx", "x");
    CHECK(first_onnx_file(root) == root / "a" / "deep.onnx");
    write_file(root / "z.onnx", "x");
    write_file(root / "m.ONNX", "x");
    write_file(root / "b.onnx.part", "x");
    CHECK(first_onnx_file(root) == root / "m.ONNX");
    // A file is not a folder to walk.
    CHECK_FALSE(first_onnx_file(root / "z.onnx"));
}

TEST_CASE("speaker_model_download_blocked reads HF_HUB_OFFLINE, then the saved policy",
          "[test_components]") {
    fv::MapEnvReader env;
    CHECK(speaker_model_download_blocked(Json::object(), env).empty());
    CHECK(speaker_model_download_blocked(Json{{"hf_access_policy", "always"}}, env).empty());
    CHECK(speaker_model_download_blocked(Json{{"hf_access_policy", "never"}}, env) ==
          "Hugging Face access is set to Never connect, so the speaker model can't download.");
    // A legacy offline flag reads as Never, as load_hf_access_policy migrates it.
    CHECK(speaker_model_download_blocked(Json{{"hf_hub_offline", true}}, env) ==
          kSpeakerModelPolicyBlocked);
    CHECK(speaker_model_download_blocked(Json{{"hf_hub_offline", false}}, env).empty());
    CHECK(speaker_model_download_blocked(Json{{"hf_access_policy", "bogus"}, {"hf_hub_offline", 1}},
                                         env) == kSpeakerModelPolicyBlocked);
    env.set("HF_HUB_OFFLINE", " yes ");
    CHECK(speaker_model_download_blocked(Json{{"hf_access_policy", "never"}}, env) ==
          "Hugging Face downloads are disabled by HF_HUB_OFFLINE, so the speaker model can't "
          "download.");
}

TEST_CASE("the pinned speaker model is OpenWhisper's", "[test_components]") {
    CHECK(kSpeakerModelUrl == "https://huggingface.co/" + std::string(kSpeakerModelRepo) +
                                  "/resolve/" + std::string(kSpeakerModelRevision) + "/" +
                                  std::string(kSpeakerModelFilename));
    CHECK(kSpeakerModelSha256 ==
          "7bb2f06e9df17cdf1ef14ee8a15ab08ed28e8d0ef5054ee135741560df2ec068");
    CHECK(kSpeakerModelSizeBytes == 26'530'309);
}

TEST_CASE("test_ensure_speaker_model_returns_existing_without_download", "[test_components]") {
    Rig rig;
    LoopbackServer server(serve(kModelBytes));
    write_file(rig.destination(), "onnx");
    auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
    CHECK(ensure_speaker_model(deps) == rig.destination());
    CHECK(server.requests().empty());
}

TEST_CASE("test_ensure_speaker_model_downloads_when_missing", "[test_components]") {
    Rig rig;
    LoopbackServer server(serve(kModelBytes));
    auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
    std::vector<std::string> phases;
    deps.progress = [&](std::string_view phase, std::int64_t, std::int64_t) {
        phases.emplace_back(phase);
    };
    const fs::path path = ensure_speaker_model(deps);
    CHECK(path == rig.destination());
    CHECK(read_file(path) == kModelBytes);
    CHECK_FALSE(fs::exists(fs::path(path) += ".part"));
    CHECK(speaker_model_error().empty());
    REQUIRE(server.requests().size() == 1);
    CHECK(server.requests()[0].target == "/voxceleb_resnet34_LM.onnx");
    CHECK_FALSE(phases.empty());
    // From now on the local file is used.
    CHECK(speaker_model_path(rig.paths, rig.env) == path);
}

TEST_CASE("a download follows Hugging Face's redirect to its file host", "[test_components]") {
    Rig rig;
    LoopbackServer files(serve(kModelBytes));
    LoopbackServer hub([&files](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(
            302, "Found", {{"Location", files.url("/cdn/voxceleb_resnet34_LM.onnx")}}, ""));
    });
    auto deps = rig.deps(hub.url("/Wespeaker/resolve/main/voxceleb_resnet34_LM.onnx"));
    CHECK(read_file(ensure_speaker_model(deps)) == kModelBytes);
    CHECK(files.requests().at(0).target == "/cdn/voxceleb_resnet34_LM.onnx");
}

TEST_CASE("test_ensure_speaker_model_skips_download_when_blocked", "[test_components]") {
    Rig rig;
    LoopbackServer server(serve(kModelBytes));
    auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
    rig.store.save_all(Json{{"hf_access_policy", "never"}});
    CHECK_THAT(message_of([&] { ensure_speaker_model(deps); }), ContainsSubstring("Never connect"));
    CHECK(server.requests().empty());
    CHECK(speaker_model_error().empty());

    rig.env.set("HF_HUB_OFFLINE", "1");
    CHECK(message_of([&] { ensure_speaker_model(deps); }) == kSpeakerModelEnvBlocked);
    CHECK(server.requests().empty());
    CHECK(speaker_model_error().empty());
}

TEST_CASE("a legacy offline flag blocks the download and is migrated", "[test_components]") {
    Rig rig;
    LoopbackServer server(serve(kModelBytes));
    auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
    rig.store.save_all(Json{{"hf_hub_offline", true}});
    CHECK(message_of([&] { ensure_speaker_model(deps); }) == kSpeakerModelPolicyBlocked);
    CHECK(rig.store.snapshot()->value("hf_access_policy", "") == "never");
    CHECK(server.requests().empty());
}

TEST_CASE("test_speaker_model_failure_is_kept_until_a_download_works", "[test_components]") {
    Rig rig;
    auto failing = rig.deps(closed_url());
    const std::string reason =
        "Could not reach Hugging Face to download the speaker model (network error).";
    CHECK(message_of([&] { ensure_speaker_model(failing); }) == reason);
    CHECK(speaker_model_error() == reason);
    // A blocked attempt is not a failure and leaves the kept one alone.
    rig.store.save_all(Json{{"hf_access_policy", "never"}});
    CHECK(message_of([&] { ensure_speaker_model(failing); }) == kSpeakerModelPolicyBlocked);
    CHECK(speaker_model_error() == reason);

    rig.store.save_all(Json{{"hf_access_policy", "ask"}});
    LoopbackServer server(serve(kModelBytes));
    auto working = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
    CHECK(ensure_speaker_model(working) == rig.destination());
    CHECK(speaker_model_error().empty());
}

TEST_CASE("test_speaker_download_errors_read_as_one_sentence", "[test_components]") {
    using Kind = SpeakerDownloadFailure::Kind;
    const auto describe = [](Kind kind, int status = 0, std::string strerror = {},
                             std::string message = {}, std::string type = {}) {
        SpeakerDownloadFailure failure;
        failure.kind = kind;
        failure.status = status;
        failure.strerror = std::move(strerror);
        failure.message = std::move(message);
        failure.type_name = std::move(type);
        return describe_speaker_download_error(failure);
    };
    CHECK_THAT(describe(Kind::Network), ContainsSubstring("network error"));
    CHECK_THAT(describe(Kind::Timeout), ContainsSubstring("did not respond in time"));
    CHECK_THAT(describe(Kind::HttpStatus, 403), ContainsSubstring("(HTTP 403)"));
    CHECK(describe(Kind::Disk, 0, "No space left on device", "[Errno 28] No space left on device",
                   "OSError") == "Could not save the speaker model: No space left on device.");
    CHECK(describe(Kind::Other, 0, {}, "boom\ntraceback", "RuntimeError") ==
          "Could not download the speaker model: boom.");
    CHECK(describe(Kind::Other, 0, {}, "", "RuntimeError") ==
          "Could not download the speaker model: RuntimeError.");
}

TEST_CASE("speaker download errors match Python's", "[golden][test_components]") {
    golden::for_each("services.components_speaker", "_describe_speaker_download_error",
                     [](const golden::Case& c) {
                         const std::string kind = c.arg<std::string>(0);
                         const Json result = c.result<Json>();
                         SpeakerDownloadFailure failure;
                         if (kind == "timeout") {
                             failure.kind = SpeakerDownloadFailure::Kind::Timeout;
                         } else if (kind == "transport") {
                             failure.kind = SpeakerDownloadFailure::Kind::Network;
                         } else if (kind == "http_status") {
                             failure.kind = SpeakerDownloadFailure::Kind::HttpStatus;
                             failure.status = c.arg<int>(2);
                         } else if (kind == "os_error") {
                             failure.kind = SpeakerDownloadFailure::Kind::Disk;
                         } else {
                             failure.kind = SpeakerDownloadFailure::Kind::Other;
                         }
                         if (result.at("strerror").is_string()) {
                             failure.strerror = result.at("strerror").get<std::string>();
                         }
                         failure.message = result.at("str").get<std::string>();
                         failure.type_name = result.at("type").get<std::string>();
                         CHECK(describe_speaker_download_error(failure) ==
                               result.at("text").get<std::string>());
                     });
}

TEST_CASE("each download failure reads as its sentence", "[test_components]") {
    Rig rig;

    SECTION("refused by the server") {
        LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
            reply.write(LoopbackServer::response(403, "Forbidden", {}, "no"));
        });
        auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
        CHECK(message_of([&] { ensure_speaker_model(deps); }) ==
              "Hugging Face refused the speaker model download (HTTP 403).");
    }
    SECTION("a file that fails its digest is discarded") {
        LoopbackServer server(serve(std::string(64, 'x')));
        auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
        CHECK(message_of([&] { ensure_speaker_model(deps); }) ==
              "The speaker model failed its integrity check and was discarded.");
        CHECK_FALSE(fs::exists(rig.destination()));
        CHECK_FALSE(fs::exists(fs::path(rig.destination()) += ".part"));
        CHECK_FALSE(speaker_model_path(rig.paths, rig.env));
    }
    SECTION("a file of another size is another file") {
        LoopbackServer server(serve(std::string(65, 'w')));
        auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
        CHECK(message_of([&] { ensure_speaker_model(deps); }) == kSpeakerModelIntegrity);
        CHECK_FALSE(fs::exists(rig.destination()));
    }
    SECTION("a connection cut short is a network error, and the bytes so far are kept") {
        LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
            reply.write(LoopbackServer::response(200, "OK", {{"Content-Length", "64"}},
                                                 std::string(16, 'w')));
        });
        auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
        CHECK(message_of([&] { ensure_speaker_model(deps); }) ==
              "Could not reach Hugging Face to download the speaker model (network error).");
        CHECK(read_file(fs::path(rig.destination()) += ".part") == std::string(16, 'w'));
    }
    SECTION("no answer in time") {
        fv::http::FakeHttpClient client;
        fv::http::ScriptedResponse timeout;
        timeout.error = fv::http::Error{fv::http::ErrorKind::Timeout, "timed out"};
        timeout.error->after_request = true;
        client.push(timeout);
        auto deps = rig.deps("https://huggingface.co/x.onnx", &client);
        CHECK(message_of([&] { ensure_speaker_model(deps); }) ==
              "Hugging Face did not respond in time, so the speaker model did not download.");
    }
    SECTION("a certificate that fails verification is a network error") {
        fv::http::FakeHttpClient client;
        fv::http::ScriptedResponse tls;
        tls.error = fv::http::Error{fv::http::ErrorKind::Tls, "certificate verify failed"};
        tls.error->certificate = true;
        client.push(tls);
        auto deps = rig.deps("https://huggingface.co/x.onnx", &client);
        CHECK(message_of([&] { ensure_speaker_model(deps); }) ==
              "Could not reach Hugging Face to download the speaker model (network error).");
    }
    SECTION("a folder that cannot be made") {
        // A file where the models folder should be.
        write_file(rig.temp.path() / "FioriVoce" / "models", "not a folder");
        LoopbackServer server(serve(kModelBytes));
        auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
        const std::string message = message_of([&] { ensure_speaker_model(deps); });
        CHECK_THAT(message, Catch::Matchers::StartsWith("Could not save the speaker model: "));
        CHECK_FALSE(message.contains("Error "));
        CHECK(server.requests().empty());
    }
    CHECK_FALSE(speaker_model_error().empty());
}

TEST_CASE("a cancelled download is not a failure and resumes next time", "[test_components]") {
    Rig rig;
    fv::CancelSource cancel;
    LoopbackServer server([&cancel](const LoopbackServer::Request& request,
                                    LoopbackServer::Reply& reply) {
        if (!request.header("Range")) {
            reply.write("HTTP/1.1 200 OK\r\nContent-Length: 64\r\nConnection: close\r\n\r\n");
            reply.write(std::string(16, 'w'));
            // Cancelled once the client has had time to take those bytes.
            reply.wait(std::chrono::milliseconds(500));
            cancel.cancel();
            reply.wait(std::chrono::milliseconds(2000));
            return;
        }
        reply.write(LoopbackServer::response(
            206, "Partial Content", {{"Content-Range", "bytes 16-63/64"}}, std::string(48, 'w')));
    });
    auto deps = rig.deps(server.url("/voxceleb_resnet34_LM.onnx"));
    CHECK_THROWS_AS(ensure_speaker_model(deps, cancel.token()), ComponentCanceled);
    CHECK(speaker_model_error().empty());
    CHECK(fs::exists(fs::path(rig.destination()) += ".part"));

    CHECK(read_file(ensure_speaker_model(deps)) == kModelBytes);
    CHECK(server.requests().back().header("Range") == "bytes=16-");
}

TEST_CASE("one speaker-model download at a time", "[test_components]") {
    Rig rig;
    std::atomic<int> served{0};
    LoopbackServer server([&served](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        ++served;
        // Long enough for the second caller to arrive while this download runs.
        reply.wait(std::chrono::milliseconds(300));
        reply.write(LoopbackServer::response(200, "OK", {}, kModelBytes));
    });
    const std::string url = server.url("/voxceleb_resnet34_LM.onnx");
    fs::path first;
    fs::path second;
    std::thread other([&] {
        auto deps = rig.deps(url);
        first = ensure_speaker_model(deps);
    });
    auto deps = rig.deps(url);
    second = ensure_speaker_model(deps);
    other.join();
    CHECK(first == rig.destination());
    CHECK(second == rig.destination());
    CHECK(served == 1);
}
