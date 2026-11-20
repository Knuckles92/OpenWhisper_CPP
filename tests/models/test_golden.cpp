// Golden vectors made by running OpenWhisper's catalog, component, cache and GPU functions
// (tools/parity/cases/services.*.py), replayed against the port.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "base/utf8.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/gpu_probe.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "models_test_support.h"
#include "pyport/json.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;
namespace golden = fv::golden;

namespace {

using Installed = std::set<std::string, std::less<>>;

std::optional<NvidiaGpu> gpu_arg(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    NvidiaGpu card;
    card.name = value.at(0).get<std::string>();
    card.total_mib = value.at(1).get<std::int64_t>();
    if (!value.at(2).is_null()) {
        card.compute_capability =
            std::pair{value.at(2).at(0).get<int>(), value.at(2).at(1).get<int>()};
    }
    return card;
}

Host host_arg(const Json& platform, const Installed* installed, const Json& card) {
    Host host;
    if (!platform.is_null()) {
        host.platform = platform.get<std::string>();
    }
    if (installed != nullptr) {
        host.is_installed = [installed](std::string_view id) {
            return installed->contains(id);
        };
    }
    if (const auto found = gpu_arg(card)) {
        host.gpu = [found] {
            return found;
        };
    }
    return host;
}

Installed installed_arg(const Json& value) {
    Installed ids;
    for (const Json& id : value) {
        ids.insert(id.get<std::string>());
    }
    return ids;
}

Json optional_text(const std::optional<std::string>& text) {
    return text ? Json(*text) : Json();
}

// The NVIDIA Speech ids among OpenWhisper's offered components: the only ones this build has.
Json nvidia_only(const Json& ids) {
    Json kept = Json::array();
    for (const Json& id : ids) {
        if (id.get<std::string>().starts_with("asr-nvidia-")) {
            kept.push_back(id);
        }
    }
    return kept;
}

} // namespace

TEST_CASE("services.local_asr.catalog.resolve_runtime vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "resolve_runtime", [](const golden::Case& c) {
        const Installed installed = installed_arg(c.args()[3]);
        const Host host = host_arg(c.args()[2], &installed, c.args()[4]);
        const auto [component, device] =
            resolve_runtime(c.arg<std::string>(0), c.arg<std::string>(1), host);
        CHECK(golden::equal(c.result<Json>(), Json::array({component, device})));
    });
}

TEST_CASE("services.local_asr.catalog.runtime_id vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "runtime_id", [](const golden::Case& c) {
        const Host host = host_arg(c.args()[2], nullptr, c.args()[3]);
        CHECK(runtime_id(c.arg<std::string>(0), c.arg<std::string>(1), host) ==
              c.result<std::string>());
    });
}

TEST_CASE("services.local_asr.catalog.nvidia_gpu_runtime vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "nvidia_gpu_runtime", [](const golden::Case& c) {
        CHECK(nvidia_gpu_runtime(host_arg(c.args()[0], nullptr, c.args()[1])) ==
              c.result<std::string>());
    });
}

TEST_CASE("services.local_asr.catalog.missing_runtime vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "missing_runtime", [](const golden::Case& c) {
        const Installed installed = installed_arg(c.args()[3]);
        const Host host = host_arg(c.args()[2], &installed, c.args()[4]);
        CHECK(golden::equal(c.result<Json>(), optional_text(missing_runtime(c.arg<std::string>(0),
                                                                            c.args()[1], host))));
    });
}

TEST_CASE("services.local_asr.catalog.selected_model vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "selected_model", [](const golden::Case& c) {
        CHECK(selected_model(c.args()[1], c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("services.local_asr.catalog.selected_device vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "selected_device", [](const golden::Case& c) {
        CHECK(selected_device(c.args()[1], c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("services.components.check_compatibility vectors", "[golden]") {
    golden::for_each("services.components", "check_compatibility", [](const golden::Case& c) {
        const Host host = host_arg(c.args()[1], nullptr, Json());
        const auto reason = check_compatibility(c.args()[0], host);
        const Json expected = c.result<Json>();
        if (expected.is_null()) {
            CHECK_FALSE(reason);
        } else {
            REQUIRE(reason);
            CHECK(golden::equal_rebranded(expected.get<std::string>(), *reason));
        }
    });
}

TEST_CASE("services.components.component_is_published vectors", "[golden]") {
    golden::for_each("services.components", "component_is_published", [](const golden::Case& c) {
        CHECK(component_is_published(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<bool>());
    });
}

TEST_CASE("services.components.available_component_ids vectors", "[golden]") {
    golden::for_each("services.components", "available_component_ids", [](const golden::Case& c) {
        const Installed installed = installed_arg(c.args()[1]);
        Host host = host_arg(Json(), &installed, c.args()[2]);
        // A computer of the platform asked (the vectors patch OpenWhisper's host tag to it).
        host.platform = c.arg<std::string>(0);
        Json ids = Json::array();
        for (const auto& id : available_component_ids(c.arg<std::string>(0), host)) {
            ids.push_back(id);
        }
        CHECK(golden::equal(nvidia_only(c.result<Json>()), ids));
    });
}

TEST_CASE("services.components.read_manifest vectors", "[golden]") {
    golden::for_each("services.components", "read_manifest", [](const golden::Case& c) {
        TempDir temp;
        fv::http::FakeHttpClient client;
        ComponentStoreOptions options;
        options.host = host_for("win_amd64");
        ComponentStore store(temp.path(), client, options);
        fs::create_directories(temp.path() / "asr-nvidia-cpu");
        if (!c.args()[0].is_null()) {
            write_file(temp.path() / "asr-nvidia-cpu" / "manifest.json", c.arg<std::string>(0));
        }
        const auto manifest = store.read_manifest("asr-nvidia-cpu");
        CHECK(golden::equal(c.result<Json>(), manifest ? *manifest : Json()));
    });
}

TEST_CASE("services.components.describe vectors", "[golden]") {
    golden::for_each("services.components", "describe", [](const golden::Case& c) {
        const Json& scenario = c.args()[0];
        TempDir temp;
        const fs::path target = temp.path() / fv::base_utf8::path_from_utf8(
                                                  scenario.at("component").get<std::string>());
        if (scenario.value("dir", false)) {
            fs::create_directories(target);
        }
        if (scenario.contains("files")) {
            for (const auto& [name, size] : scenario.at("files").items()) {
                write_file(target / fv::base_utf8::path_from_utf8(name),
                           std::string(size.get<std::size_t>(), 'x'));
            }
        }
        if (scenario.contains("manifest") && !scenario.at("manifest").is_null()) {
            fs::create_directories(target);
            write_file(target / "manifest.json", scenario.at("manifest").get<std::string>());
        }
        if (scenario.contains("sentinel") && !scenario.at("sentinel").is_null()) {
            fs::create_directories(target);
            write_file(target / ".installed", scenario.at("sentinel").get<std::string>());
        }
        fv::http::FakeHttpClient client;
        ComponentStoreOptions options;
        options.host = host_for(scenario.at("platform").get<std::string>());
        ComponentStore store(temp.path(), client, options);
        const Json catalog = scenario.at("catalog");
        ComponentCoordinator::Options coordinator_options;
        coordinator_options.catalog = &catalog;
        const auto info = ComponentCoordinator(store, coordinator_options)
                              .describe(scenario.at("component").get<std::string>());
        const Json expected = c.result<Json>();
        CHECK(info.component_id == expected.at("component_id").get<std::string>());
        CHECK(state_name(info.state) == expected.at("state").get<std::string>());
        CHECK(golden::equal(expected.at("installed_version"),
                            info.installed_version.value_or(Json())));
        CHECK(golden::equal(expected.at("available_version"),
                            info.available_version.value_or(Json())));
        CHECK(info.download_bytes == expected.at("download_bytes").get<std::int64_t>());
        CHECK(info.install_bytes == expected.at("install_bytes").get<std::int64_t>());
        CHECK(golden::equal_rebranded(expected.at("reason").get<std::string>(), info.reason));
        CHECK(info.is_usable() == expected.at("is_usable").get<bool>());
    });
}

TEST_CASE("services.local_asr.cache.is_cached vectors", "[golden]") {
    const Json spec = golden::load("services.local_asr.cache").at("pins").at("spec");
    golden::for_each("services.local_asr.cache", "is_cached", [&spec](const golden::Case& c) {
        TempDir temp;
        const fs::path folder = temp.path() / "parakeet-v3";
        fs::create_directories(folder);
        if (!c.args()[0].is_null()) {
            write_file(folder / "installed.json", c.arg<std::string>(0));
        }
        for (const auto& [name, size] : c.args()[1].items()) {
            write_file(folder / name, std::string(size.get<std::size_t>(), 'x'));
        }
        CHECK(is_model_folder_complete(folder, spec) == c.result<bool>());
    });
}

TEST_CASE("services.gpu_info.short_name vectors", "[golden]") {
    golden::for_each("services.gpu_info", "short_name", [](const golden::Case& c) {
        NvidiaGpu card;
        card.name = c.arg<std::string>(0);
        CHECK(card.short_name() == c.result<std::string>());
    });
}

TEST_CASE("services.gpu_info.format_gb vectors", "[golden]") {
    golden::for_each("services.gpu_info", "format_gb", [](const golden::Case& c) {
        CHECK(format_gb(c.arg<std::int64_t>(0)) == c.result<std::string>());
    });
}
