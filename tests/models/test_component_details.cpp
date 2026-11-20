// The bundled speech-runtime profiles: four tests of OpenWhisper's tests/test_component_catalog.py
// and the vectors of services.component_catalog, with OpenWhisper_CPP's rewrites spelled out.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "models/component_details.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/runtime_resolver.h"
#include "models_test_support.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;
namespace golden = fv::golden;

namespace {

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// OpenWhisper's row as OpenWhisper_CPP words it: only Parakeet among the engines, the runtime
// loaded in process (no worker process, no portable Python, no Python licence or download link),
// and the product renamed.
Json openwhisper_cpp_row(Json row) {
    const auto engines = [](const Json& text) {
        return replaced(text.get<std::string>(), "Parakeet and Nemotron", "Parakeet models");
    };
    row["summary"] = engines(row.at("summary"));
    row["best_for"] = engines(row.at("best_for"));
    row["description"] = replaced(engines(row.at("description")), " Runs in a dedicated process.",
                                  " Runs inside OpenWhisper_CPP.");
    row["payload"] = "Verified native libraries";
    CHECK(row.at("local_format") == "Isolated worker process");
    row["local_format"] = "Native libraries loaded by OpenWhisper_CPP";
    row["license"] = replaced(row.at("license").get<std::string>(), "; Python PSF", "");
    Json urls = Json::array();
    for (const Json& url : row.at("source_urls")) {
        if (!url.get<std::string>().starts_with("https://www.python.org/")) {
            urls.push_back(url);
        }
    }
    row["source_urls"] = urls;
    row["maintainer"] = golden::rebrand(row.at("maintainer").get<std::string>());
    return row;
}

Json details_json(const ComponentDetails& d) {
    return Json{{"component_id", d.component_id}, {"display_name", d.display_name},
                {"summary", d.summary},           {"description", d.description},
                {"origin_name", d.origin_name},   {"origin_url", d.origin_url},
                {"origin_label", d.origin_label}, {"source_name", d.source_name},
                {"source_url", d.source_url},     {"source_label", d.source_label},
                {"maintainer", d.maintainer},     {"family", d.family},
                {"requires", d.requires_text},    {"payload", d.payload},
                {"local_format", d.local_format}, {"license", d.license},
                {"best_for", d.best_for},         {"limitations", d.limitations},
                {"compact_tags", d.compact_tags}, {"source_note", d.source_note},
                {"source_urls", d.source_urls}};
}

} // namespace

TEST_CASE("TestComponentCatalog::test_catalog_covers_every_component_id",
          "[test_component_catalog]") {
    // from tests/test_component_catalog.py: the speech runtimes (GPU Acceleration and the
    // meeting agents are not OpenWhisper_CPP components).
    std::set<std::string> expected(runtime_ids().begin(), runtime_ids().end());
    std::set<std::string> described;
    for (const ComponentDetails& details : component_catalog()) {
        described.insert(details.component_id);
    }
    CHECK(described == expected);
    CHECK_THROWS_AS(get_component_details("not-a-component"), std::out_of_range);
}

TEST_CASE("TestComponentCatalog::test_every_entry_is_complete_and_uses_https_sources",
          "[test_component_catalog]") {
    for (const ComponentDetails& details : component_catalog()) {
        INFO(details.component_id);
        for (const std::string* field :
             {&details.display_name, &details.summary, &details.description, &details.origin_name,
              &details.origin_url, &details.origin_label, &details.source_name, &details.source_url,
              &details.source_label, &details.maintainer, &details.family, &details.requires_text,
              &details.payload, &details.local_format, &details.license, &details.best_for,
              &details.compact_tags, &details.source_note}) {
            CHECK(field->find_first_not_of(" \t\n") != std::string::npos);
        }
        CHECK_FALSE(details.limitations.empty());
        // OpenWhisper's rows have two links: the project and the portable Python's download
        // page, which OpenWhisper_CPP does not install.
        CHECK(details.source_urls.size() == 1);
        CHECK(details.origin_url.starts_with("https://"));
        CHECK(details.source_url.starts_with("https://"));
        for (const std::string& url : details.source_urls) {
            CHECK(url.starts_with("https://"));
        }
        CHECK(details.description != details.summary);
    }
}

TEST_CASE("TestComponentCatalog::test_describe_uses_the_catalog_copy", "[test_component_catalog]") {
    TempDir temp;
    fv::http::FakeHttpClient client;
    ComponentStoreOptions options;
    options.host = host_for("win_amd64");
    ComponentStore store(temp.path(), client, options);
    ComponentCoordinator::Options coordinator_options;
    coordinator_options.copy_for = component_copy;
    const ComponentCoordinator coordinator(store, coordinator_options);
    const ComponentDetails& details = get_component_details(component_id::kAsrNvidiaCuda);
    const ComponentInfo info = coordinator.describe(component_id::kAsrNvidiaCuda);
    CHECK(info.display_name == details.display_name);
    CHECK(info.summary == details.summary);
    CHECK(component_copy("not-a-component") ==
          std::pair<std::string, std::string>{"not-a-component", ""});
}

TEST_CASE("TestComponentCatalog::test_catalog_entries_are_immutable", "[test_component_catalog]") {
    static_assert(
        std::is_same_v<decltype(get_component_details("asr-nvidia-cpu")), const ComponentDetails&>);
    static_assert(std::is_same_v<decltype(component_catalog()), std::span<const ComponentDetails>>);
    CHECK(&get_component_details("asr-nvidia-cpu") == &get_component_details("asr-nvidia-cpu"));
}

TEST_CASE("services.component_catalog.get_component_details vectors", "[golden]") {
    golden::for_each("services.component_catalog", "get_component_details",
                     [](const golden::Case& c) {
                         const std::string id = c.arg<std::string>(0);
                         if (c.raises()) {
                             CHECK(c.raises_type() == "KeyError");
                             CHECK_THROWS_AS(get_component_details(id), std::out_of_range);
                             return;
                         }
                         CHECK(golden::equal(openwhisper_cpp_row(c.result<Json>()),
                                             details_json(get_component_details(id))));
                     });
}
