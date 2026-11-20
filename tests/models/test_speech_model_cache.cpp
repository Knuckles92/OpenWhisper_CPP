// The speech-model cache and the speech-runtime swap: the cache half of OpenWhisper's
// tests/test_local_asr.py, with its isolated_models fixture (a small pinned file standing in for
// the model, and a download that writes it).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include "base/hash.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "models/component_store.h"
#include "models/speech_model_cache.h"
#include "models/testing/fake_openwhisper.h"
#include "models_test_support.h"
#include "pyport/json.h"

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

const std::string kData = "verified model";

// isolated_models: every key pins one small weights.gguf, and a download writes it.
struct IsolatedModels {
    TempDir temp;
    fv::http::FakeHttpClient client;
    fv::MapEnvReader env;
    Json spec = testing::small_model_spec("weights.gguf", kData);
    SpeechModelCacheOptions options;

    IsolatedModels() {
        options.env = &env;
        options.artifacts = [this](std::string_view) {
            return spec;
        };
        options.download_file = [](std::string_view, std::string_view sha, std::int64_t size,
                                   const fs::path& target, const InstallProgress&,
                                   const fv::CancelToken&, std::int64_t, std::int64_t) {
            CHECK(sha == fv::sha256_hex(kData));
            CHECK(size == static_cast<std::int64_t>(kData.size()));
            write_file(target, kData);
        };
    }

    SpeechModelCache cache() { return SpeechModelCache(temp.path(), client, options); }
};

fs::filesystem_error os_error(std::errc code, std::string what, const fs::path& path = {}) {
    return fs::filesystem_error(what, path, std::make_error_code(code));
}

} // namespace

TEST_CASE("test_partial_cache_is_not_loadable_and_download_finishes_atomically",
          "[test_local_asr]") {
    IsolatedModels isolated;
    auto cache = isolated.cache();
    const std::string key = "parakeet-v3";
    const auto partial = cache.model_dir(key).parent_path() / (key + ".partial");
    write_file(partial / "weights.gguf", "broken");
    CHECK_FALSE(cache.is_cached(key));
    cache.download(key);
    CHECK(cache.is_cached(key));
    CHECK(cache.load_path(key).filename() == "weights.gguf");
    CHECK_FALSE(fs::exists(partial));
    write_file(cache.model_dir(key) / "weights.gguf", "truncated");
    CHECK_FALSE(cache.is_cached(key));
}

TEST_CASE("test_hard_offline_blocks_all_model_hosts", "[test_local_asr]") {
    IsolatedModels isolated;
    isolated.env.set("HF_HUB_OFFLINE", "1");
    auto cache = isolated.cache();
    CHECK_THROWS_MATCHES(cache.download("orukeet-v0.1"), ModelCacheError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("HF_HUB_OFFLINE")));
    CHECK_FALSE(cache.is_cached("orukeet-v0.1"));
}

TEST_CASE("test_failed_download_cannot_publish_cache", "[test_local_asr]") {
    IsolatedModels isolated;
    isolated.options.download_file = [](std::string_view, std::string_view, std::int64_t,
                                        const fs::path&, const InstallProgress&,
                                        const fv::CancelToken&, std::int64_t, std::int64_t) {
        throw os_error(std::errc::network_unreachable, "network interrupted");
    };
    auto cache = isolated.cache();
    CHECK_THROWS_AS(cache.download("parakeet-v3"), std::system_error);
    CHECK_FALSE(cache.is_cached("parakeet-v3"));
}

TEST_CASE("test_cache_swap_rolls_back_previous_tree", "[test_local_asr]") {
    IsolatedModels isolated;
    isolated.options.replace = [](const fs::path& from, const fs::path& to) {
        if (fv::base_utf8::path_to_utf8(from).ends_with(".partial")) {
            throw os_error(std::errc::permission_denied, "locked", from);
        }
        fs::rename(from, to);
    };
    auto cache = isolated.cache();
    const auto target = cache.model_dir("parakeet-v3");
    write_file(target / "old.txt", "keep");
    CHECK_THROWS_WITH(cache.download("parakeet-v3"), ContainsSubstring("locked"));
    CHECK(read_file(target / "old.txt") == "keep");
}

TEST_CASE("test_native_runtime_swap_retries_short_windows_lock", "[test_local_asr]") {
    TempDir temp;
    fv::http::FakeHttpClient client;
    const auto source = temp.path() / "staging";
    const auto target = temp.path() / "installed";
    write_file(source / "model", "ready");
    int attempts = 0;
    ComponentStoreOptions options;
    options.host = host_for("win_amd64");
    options.runtime_swap_poll = std::chrono::milliseconds(1);
    options.file_ops.replace = [&attempts](const fs::path& from, const fs::path& to) {
        if (++attempts < 3) {
            // A scanner still reading the files: Windows' sharing violation.
            throw fs::filesystem_error("scanner still reading files", from, to,
                                       std::make_error_code(std::errc::permission_denied));
        }
        fs::rename(from, to);
    };
    ComponentStore store(temp.path() / "components", client, options);
    store.replace_speech_runtime(source, target, {});
    CHECK(read_file(target / "model") == "ready");
    CHECK(attempts == 3);
}

TEST_CASE("test_native_runtime_swap_cancellation_keeps_staging", "[test_local_asr]") {
    TempDir temp;
    fv::http::FakeHttpClient client;
    const auto source = temp.path() / "staging";
    const auto target = temp.path() / "installed";
    fs::create_directories(source);
    ComponentStoreOptions options;
    options.host = host_for("win_amd64");
    ComponentStore store(temp.path() / "components", client, options);
    fv::CancelSource canceled;
    canceled.cancel();
    CHECK_THROWS_AS(store.replace_speech_runtime(source, target, canceled.token()),
                    ComponentCanceled);
    CHECK(fs::exists(source));
    CHECK_FALSE(fs::exists(target));
}

// The rest of cache.py: removal, the inventory, and local installs (adoption and import).

TEST_CASE("remove deletes the model with its partial and previous folders", "[models]") {
    IsolatedModels isolated;
    auto cache = isolated.cache();
    cache.download("parakeet-v3");
    const auto target = cache.model_dir("parakeet-v3");
    write_file(target.parent_path() / "parakeet-v3.partial" / "x", "x");
    write_file(target.parent_path() / "parakeet-v3.previous" / "x", "x");
    const auto donor = isolated.temp.path() / "donor" / "weights.gguf";
    fs::create_directories(donor.parent_path());
    fs::create_hard_link(target / "weights.gguf", donor);
    REQUIRE(fs::hard_link_count(donor) == 2);
    cache.remove("parakeet-v3");
    CHECK_FALSE(fs::exists(target));
    CHECK(read_file(donor) == kData);
    CHECK(fs::hard_link_count(donor) == 1);
    CHECK_FALSE(fs::exists(target.parent_path() / "parakeet-v3.partial"));
    CHECK_FALSE(fs::exists(target.parent_path() / "parakeet-v3.previous"));
}

TEST_CASE("an interrupted swap's previous copy comes back before a download", "[models]") {
    IsolatedModels isolated;
    auto cache = isolated.cache();
    cache.download("parakeet-v3");
    const auto target = cache.model_dir("parakeet-v3");
    fs::rename(target, target.parent_path() / "parakeet-v3.previous");
    int downloads = 0;
    isolated.options.download_file =
        [&downloads](std::string_view, std::string_view, std::int64_t, const fs::path&,
                     const InstallProgress&, const fv::CancelToken&, std::int64_t, std::int64_t) {
            ++downloads;
        };
    auto again = isolated.cache();
    again.download("parakeet-v3");
    CHECK(downloads == 0);
    CHECK(again.is_cached("parakeet-v3"));
}

TEST_CASE("the inventory lists cached models by repository", "[models]") {
    IsolatedModels isolated;
    auto cache = isolated.cache();
    CHECK(cache.inventory().empty());
    cache.download("orukeet-v0.1");
    const auto listed = cache.inventory();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].repo_id == "test/model");
    CHECK(listed[0].size_on_disk == static_cast<std::int64_t>(kData.size()));
    CHECK(listed[0].path == cache.model_dir("orukeet-v0.1"));
    CHECK(listed[0].revisions == std::vector<std::string>{"pinned"});
}

TEST_CASE("load_path refuses a model that is not downloaded", "[models]") {
    IsolatedModels isolated;
    auto cache = isolated.cache();
    CHECK_THROWS_WITH(cache.load_path("parakeet-v3"),
                      "Parakeet TDT 0.6B v3 is not downloaded. Open Downloads.");
    CHECK_THROWS_WITH(cache.model_dir("turbo"), "Unknown speech model");
}

TEST_CASE("a local install publishes only files that match the pin", "[models]") {
    IsolatedModels isolated;
    auto cache = isolated.cache();
    CHECK_THROWS_WITH(
        cache.install_local(
            "parakeet-v3",
            [](const fs::path& staging) { write_file(staging / "weights.gguf", "verified modeL"); },
            "mismatch"),
        "mismatch");
    CHECK_FALSE(cache.is_cached("parakeet-v3"));
    CHECK_FALSE(fs::exists(cache.model_dir("parakeet-v3").parent_path() / "parakeet-v3.partial"));
    cache.install_local(
        "parakeet-v3", [](const fs::path& staging) { write_file(staging / "weights.gguf", kData); },
        "mismatch");
    CHECK(cache.is_cached("parakeet-v3"));
    // installed.json is json.dumps of the spec, on one line.
    CHECK(read_file(cache.model_dir("parakeet-v3") / "installed.json") ==
          fv::py::dumps(isolated.spec));
}
