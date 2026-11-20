// A delete whose request passed its checks, with a load or a meeting starting before the worker
// removes the files: the worker's recheck (EngineControl::with_model_unused, the speech service's
// model-assets lock in the app) keeps them.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "model_runtime_harness.h"

namespace fs = std::filesystem;
using namespace fv::models::test;
using fv::Json;

TEST_CASE("a load starting between the delete's check and its worker keeps the files", "[models]") {
    RuntimeHarness h;
    h.build();
    h.cache_model(kOrukeet);

    h.on_ui([&] { h.runtime->request_delete(kOrukeet); });
    REQUIRE(h.work.queued() == 1);
    // The engine loads the model before the worker runs.
    h.engine.set_loaded_model(kOrukeet);
    h.settle();

    CHECK(h.cache->is_cached(kOrukeet));
    CHECK(h.entries("model_deleted") ==
          Json::array({Json::array(
              {"model_deleted", kOrukeet, false, "Model is in use — switch models first"})}));
    CHECK(h.count("model_cache_changed") == 0);
    CHECK(h.access->requests_in_flight() == 0);
}

TEST_CASE("a meeting starting between the delete's check and its worker keeps the files",
          "[models]") {
    RuntimeHarness h;
    h.build();
    h.cache_model(kOrukeet);

    h.on_ui([&] { h.runtime->request_delete(kOrukeet); });
    REQUIRE(h.work.queued() == 1);
    h.engine.set_meeting_active(true);
    h.settle();

    CHECK(h.cache->is_cached(kOrukeet));
    CHECK(h.entries("model_deleted").size() == 1);
    CHECK(h.entries("model_deleted")[0][2] == false);
}
