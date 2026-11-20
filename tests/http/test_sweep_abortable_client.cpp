// AbortableClient: Quit's "abort every transfer". A request waiting on a server that never
// answers ends within a second or two of abort_all(), whatever its own timeouts.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "http/abortable_client.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "loopback_server.h"

using fv::testing::LoopbackServer;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<fv::http::AbortableClient> abortable() {
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return std::make_shared<fv::http::AbortableClient>(fv::http::make_curl_client(options));
}

// A server that reads the request and never answers.
LoopbackServer::Handler silent() {
    return [](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.wait(60s);
    };
}

} // namespace

TEST_CASE("abort_all ends a request that waits on a silent server", "[curl_client]") {
    LoopbackServer server(silent());
    const auto client = abortable();
    fv::http::Request request;
    request.url = server.url("/hang");
    request.idle_timeout = 120s;
    auto pending = std::async(std::launch::async, [&] { return client->send(request, {}); });
    // Still waiting: neither timeout is anywhere near.
    REQUIRE(pending.wait_for(300ms) == std::future_status::timeout);

    const auto aborted = std::chrono::steady_clock::now();
    client->abort_all();
    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - aborted < 2500ms);
    const auto response = pending.get();
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Cancelled);
    CHECK(client->aborted());

    // Every later request is refused at once.
    const auto later = client->send(request, {});
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error().kind == fv::http::ErrorKind::Cancelled);
}

TEST_CASE("an abortable client still honours its caller's cancel", "[curl_client]") {
    LoopbackServer server(silent());
    const auto client = abortable();
    fv::http::Request request;
    request.url = server.url("/hang");
    fv::CancelSource cancel;
    auto pending =
        std::async(std::launch::async, [&] { return client->send(request, cancel.token()); });
    REQUIRE(pending.wait_for(300ms) == std::future_status::timeout);
    cancel.cancel();
    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    const auto response = pending.get();
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Cancelled);
    CHECK_FALSE(client->aborted());
}

TEST_CASE("an abortable client passes requests and answers through", "[unit]") {
    auto fake = std::make_shared<fv::http::FakeHttpClient>();
    fake->push(fv::http::ScriptedResponse{.status = 201, .reason = "Created", .body = "made"});
    fv::http::AbortableClient client(fake);
    fv::http::Request request;
    request.method = "POST";
    request.url = "http://127.0.0.1:9/items";
    const auto response = client.send(request, {});
    REQUIRE(response.has_value());
    CHECK(response->status == 201);
    CHECK(response->body == "made");
    REQUIRE(fake->requests().size() == 1);
    CHECK(fake->requests().front().url == request.url);
    CHECK_THROWS_AS(fv::http::AbortableClient(nullptr), std::invalid_argument);
}
