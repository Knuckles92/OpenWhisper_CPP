// services/text_llm.py's snapshot_from_profile over a server the user runs
// (tests/test_local_text_servers.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "http/testing/fake_http_client.h"
#include "llm/local_servers.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

TEST_CASE("a meeting snapshot records what the server reports", "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_a_meeting_snapshot_records_what_the_server_reports
    //
    // A fresh process: nothing learned yet, so the snapshot itself asks the server.
    forget_server_specs();
    http::FakeHttpClient client;
    client.set_handler(fixture_server(load_fixture("ollama")));
    TestKeys keys;
    const auto saved =
        snapshot_from_profile(*get_profile("ollama", Json::object()), "qwen3", client, keys.sources)
            .model_metadata;
    REQUIRE(saved);
    CHECK(saved->at("context_window") == 16384);
    CHECK(saved->at("thinking_levels") == Json{{"off", "none"}});
    std::vector<std::string> paths;
    for (const auto& request : client.requests()) {
        paths.push_back(url_path(request.url));
    }
    CHECK(std::ranges::count(paths, "/v1/models") == 1);
    forget_server_specs();
}
