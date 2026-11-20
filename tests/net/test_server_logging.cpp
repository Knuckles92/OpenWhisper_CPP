// The server must not log a peer's connection reset while closing as an error
// (services/asyncio_utils.quiet_connection_lost, tests/test_asyncio_utils.py).

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <system_error>

#include "net/detail/server_log.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "net_test_support.h"

using namespace fv;
using namespace fv::net;
using fv::net::testing::ScopedCapture;

// from tests/test_asyncio_utils.py::test_reset_while_closing_a_connection_is_not_an_error
TEST_CASE("reset while closing a connection is not an error", "[test_asyncio_utils]") {
    ScopedCapture capture;
    net::detail::log_connection_error(
        net::detail::ErrorSite::Closing, true,
        "An existing connection was forcibly closed by the remote host");

    bool error_logged = false;
    bool reset_mentioned = false;
    for (const auto& line : capture->lines()) {
        error_logged = error_logged || line.level >= log::Level::Error;
        if (line.message.find("reset the connection") != std::string::npos) {
            reset_mentioned = true;
            CHECK(line.level == log::Level::Debug);
            CHECK(line.logger == "services.asyncio_utils");
        }
    }
    CHECK_FALSE(error_logged);
    CHECK(reset_mentioned);
}

// from tests/test_asyncio_utils.py::test_other_callback_errors_still_log_as_errors
TEST_CASE("other callback errors still log as errors", "[test_asyncio_utils]") {
    ScopedCapture capture;
    net::detail::log_connection_error(net::detail::ErrorSite::Closing, false, "a real bug");

    bool asyncio_error = false;
    for (const auto& line : capture->lines()) {
        asyncio_error =
            asyncio_error || (line.level >= log::Level::Error && line.logger == "asyncio");
    }
    CHECK(asyncio_error);
}

// from tests/test_asyncio_utils.py::test_a_reset_from_another_callback_still_logs_as_an_error
TEST_CASE("a reset from another callback still logs as an error", "[test_asyncio_utils]") {
    ScopedCapture capture;
    net::detail::log_connection_error(
        net::detail::ErrorSite::Other, true,
        "An existing connection was forcibly closed by the remote host");
    CHECK(capture->any(log::Level::Error));
}

TEST_CASE("a handler that throws is logged as an error and answered with 500", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    server->route("GET", "/boom", [](const HttpRequest&, std::shared_ptr<Responder>) {
        throw std::runtime_error("a real bug");
    });
    const auto endpoint = testing::listen_loopback(*server);

    ScopedCapture capture;
    const auto response = testing::loopback_client()->send(
        testing::request("GET", testing::url(endpoint.port, "/boom")), CancelToken());
    server->stop(std::chrono::milliseconds(0));

    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(response->body == "Internal Server Error");
    CHECK(http::find_header(response->headers, "connection") == "close");
    bool logged = false;
    for (const auto& line : capture->lines()) {
        if (line.level == log::Level::Error && line.logger == "uvicorn.error" &&
            line.message == "Exception in ASGI application") {
            logged = true;
            CHECK(line.exception_type.find("runtime_error") != std::string::npos);
        }
    }
    CHECK(logged);
}

TEST_CASE("a connection reset raised by a handler is still an error", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    server->route("GET", "/reset", [](const HttpRequest&, std::shared_ptr<Responder>) {
        throw std::system_error(std::make_error_code(std::errc::connection_reset));
    });
    const auto endpoint = testing::listen_loopback(*server);

    ScopedCapture capture;
    const auto response = testing::loopback_client()->send(
        testing::request("GET", testing::url(endpoint.port, "/reset")), CancelToken());
    server->stop(std::chrono::milliseconds(0));

    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(capture->any(log::Level::Error));
}

TEST_CASE("clients that hang up are not errors", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    server->route("GET", "/", [](const HttpRequest&, std::shared_ptr<Responder> responder) {
        responder->send(200, {}, "ok");
    });
    const auto endpoint = testing::listen_loopback(*server);

    ScopedCapture capture;
    {
        testing::RawClient idle(endpoint.port); // connects and leaves without a request
    }
    {
        testing::RawClient partial(endpoint.port);
        partial.send("GET / HTTP/1.1\r\nHost: x"); // leaves mid-header
    }
    {
        testing::RawClient done(endpoint.port);
        done.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK_FALSE(done.read_response().empty());
    }
    server->stop(std::chrono::milliseconds(500));
    CHECK_FALSE(capture->any(log::Level::Warning));
}
