// The bundled speech-model profiles: five tests of OpenWhisper's tests/test_model_catalog.py,
// the Orukeet test of tests/test_model_download_safety.py, and the vectors of
// services.model_catalog.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "golden/golden.h"
#include "models/consent_text.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models/model_details.h"
#include "models/speech_catalog.h"

using namespace fv::models;
using fv::Json;
namespace golden = fv::golden;

namespace {

Json details_json(const ModelDetails& d) {
    return Json{{"model_name", d.model_name},
                {"description", d.description},
                {"origin_name", d.origin_name},
                {"origin_url", d.origin_url},
                {"repository_id", d.repository_id},
                {"repository_url", d.repository_url},
                {"maintainer", d.maintainer},
                {"family", d.family},
                {"language_support", d.language_support},
                {"task_support", d.task_support},
                {"parameter_count", d.parameter_count},
                {"relative_performance", d.relative_performance},
                {"memory_guidance", d.memory_guidance},
                {"download_size_mb", d.download_size_mb},
                {"runtime_format", d.runtime_format},
                {"license", d.license},
                {"best_for", d.best_for},
                {"limitations", d.limitations},
                {"source_urls", d.source_urls},
                {"license_url", d.license_url},
                {"revision", d.revision},
                {"download_hosts", d.download_hosts},
                {"verification", d.verification},
                {"download_size", d.download_size()},
                {"compact_tags", d.compact_tags()}};
}

} // namespace

TEST_CASE("TestModelCatalog::test_catalog_covers_every_concrete_model", "[test_model_catalog]") {
    // from tests/test_model_catalog.py: the catalog is exactly the speech models (Whisper's
    // names are not in OpenWhisper_CPP).
    std::set<std::string> expected;
    for (const SpeechModel& model : models()) {
        expected.emplace(model.key);
    }
    std::set<std::string> described;
    for (const ModelDetails& details : model_catalog()) {
        described.insert(details.model_name);
    }
    CHECK(described == expected);
    CHECK_THROWS_AS(get_model_details("auto"), std::out_of_range);
}

TEST_CASE("TestModelCatalog::test_every_entry_is_complete_and_uses_https_sources",
          "[test_model_catalog]") {
    for (const ModelDetails& details : model_catalog()) {
        INFO(details.model_name);
        for (const std::string* field :
             {&details.model_name, &details.description, &details.origin_name, &details.origin_url,
              &details.repository_id, &details.repository_url, &details.maintainer, &details.family,
              &details.language_support, &details.task_support, &details.parameter_count,
              &details.relative_performance, &details.memory_guidance, &details.runtime_format,
              &details.license, &details.best_for}) {
            CHECK(field->find_first_not_of(" \t\n") != std::string::npos);
        }
        CHECK(details.download_size_mb > 0);
        CHECK_FALSE(details.limitations.empty());
        CHECK(details.source_urls.size() >= 2);
        CHECK(details.origin_url.starts_with("https://"));
        CHECK(details.repository_url.starts_with("https://"));
        for (const std::string& url : details.source_urls) {
            CHECK(url.starts_with("https://"));
        }
    }
}

TEST_CASE("TestModelCatalog::test_repository_and_size_match_download_configuration",
          "[test_model_catalog]") {
    for (const ModelDetails& details : model_catalog()) {
        INFO(details.model_name);
        CHECK(details.repository_id == artifacts(details.model_name).at("repo").get<std::string>());
        CHECK(format_download_size(details.model_name) == details.download_size());
    }
}

TEST_CASE("TestModelCatalog::test_parameter_counts_match_published_model_families",
          "[test_model_catalog]") {
    // OpenWhisper's table is Whisper's; the speech models' counts are these.
    CHECK(get_model_details("parakeet-v3").parameter_count == "600 million");
    CHECK(get_model_details("orukeet-v0.1").parameter_count == "627 million");
}

TEST_CASE("TestModelCatalog::test_catalog_entries_are_immutable", "[test_model_catalog]") {
    static_assert(std::is_same_v<decltype(get_model_details("parakeet-v3")), const ModelDetails&>);
    static_assert(std::is_same_v<decltype(model_catalog()), std::span<const ModelDetails>>);
    CHECK(&get_model_details("parakeet-v3") == &get_model_details("parakeet-v3"));
}

TEST_CASE("test_orukeet_metadata_and_notice_identify_actual_weight_terms",
          "[test_model_download_safety]") {
    const ModelDetails& details = get_model_details("orukeet-v0.1");
    CHECK(details.license == "CC-BY-SA-4.0");
    CHECK(details.maintainer.find("Oruk AI") != std::string::npos);
    CHECK(details.maintainer.find("NVIDIA") != std::string::npos);
    CHECK(std::string(model("orukeet-v0.1")->label).find("Russian SOTA") == std::string::npos);
    const std::string body = consent_body("orukeet-v0.1", hf_access_policy::kAsk, false, {});
    CHECK(body.find(details.license_url) != std::string::npos);
    CHECK(body.find("SHA-256") != std::string::npos);
    CHECK(body.find("do not guarantee") != std::string::npos);
}

TEST_CASE("the security notices name OpenWhisper_CPP", "[test_model_catalog]") {
    CHECK(model_security_notice().find("Keep OpenWhisper_CPP and its runtimes updated") !=
          std::string::npos);
    CHECK(
        custom_model_notice().starts_with("This source has not been reviewed by OpenWhisper_CPP."));
    CHECK(get_model_details("parakeet-v3").download_size() == "~714 MB");
    CHECK_FALSE(model_download_size_mb("unknown-model"));
    CHECK(find_model_details("unknown-model") == nullptr);
}

TEST_CASE("services.model_catalog.get_model_details vectors", "[golden]") {
    golden::for_each("services.model_catalog", "get_model_details", [](const golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_THROWS_AS(get_model_details(name), std::out_of_range);
            return;
        }
        CHECK(golden::equal(c.result<Json>(), details_json(get_model_details(name))));
    });
}

TEST_CASE("services.model_catalog.format_download_mb vectors", "[golden]") {
    golden::for_each("services.model_catalog", "format_download_mb", [](const golden::Case& c) {
        CHECK(format_download_mb(c.arg<double>(0)) == c.result<std::string>());
    });
}
