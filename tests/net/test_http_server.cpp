// HttpServer over loopback with the curl client of fv_http and raw sockets: routing, bodies,
// answers from other threads, keep-alive and concurrency.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/executor.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "net_test_support.h"

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;

namespace {

struct Fixture {
    Reactor reactor;
    std::unique_ptr<HttpServer> server = HttpServer::create(reactor);
    std::shared_ptr<http::Client> client = t::loopback_client();

    Endpoint listen() { return t::listen_loopback(*server); }
    ~Fixture() { server->stop(0ms); }
};

std::string param(const HttpRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.params) {
        if (key == name) {
            return value;
        }
    }
    return "<missing>";
}

} // namespace

TEST_CASE("exact and parameter routes answer", "[platform]") {
    Fixture f;
    f.server->route("GET", "/v1/transcriptions",
                    [](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        responder->send(200, {{"content-type", "text/plain"}},
                                        "list " + request.path);
                    });
    f.server->route("GET", "/v1/transcriptions/{record_id}",
                    [](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        responder->send(200, {}, "one " + param(request, "record_id"));
                    });
    f.server->route("GET", "/v1/meetings/{meeting_id}/segments/{segment_id}",
                    [](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        responder->send(200, {},
                                        param(request, "meeting_id") + "/" +
                                            param(request, "segment_id"));
                    });
    const auto endpoint = f.listen();
    const auto get = [&](std::string path) {
        auto response = f.client->send(t::request("GET", t::url(endpoint.port, path)), {});
        REQUIRE(response);
        return *response;
    };

    auto list = get("/v1/transcriptions");
    CHECK(list.status == 200);
    CHECK(list.body == "list /v1/transcriptions");
    CHECK(http::find_header(list.headers, "content-type") == "text/plain");
    CHECK(http::find_header(list.headers, "content-length") == "23");
    CHECK(http::find_header(list.headers, "date").has_value());

    CHECK(get("/v1/transcriptions/42").body == "one 42");
    CHECK(get("/v1/transcriptions/a%20b").body == "one a b"); // params are decoded
    CHECK(get("/v1/meetings/7/segments/9").body == "7/9");
}

TEST_CASE("the request carries method, decoded query, headers and peer", "[platform]") {
    Fixture f;
    std::promise<HttpRequest> seen;
    f.server->route("POST", "/echo",
                    [&](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        seen.set_value(request);
                        responder->send(204, {}, {});
                    });
    const auto endpoint = f.listen();
    auto request =
        t::request("POST", t::url(endpoint.port, "/echo?q=a+b%21&empty=&flag&q=2"), "payload");
    request.headers = {{"X-OpenWhisper_CPP-Client", "test"}};
    const auto response = f.client->send(request, {});
    REQUIRE(response);
    CHECK(response->status == 204);
    CHECK_FALSE(http::find_header(response->headers, "content-length").has_value());

    const HttpRequest got = seen.get_future().get();
    CHECK(got.method == "POST");
    CHECK(got.path == "/echo");
    CHECK(got.body == "payload");
    CHECK(got.query == Query{{"q", "a b!"}, {"empty", ""}, {"flag", ""}, {"q", "2"}});
    CHECK(header(got.headers, "x-openwhisper-cpp-client") == "test");
    CHECK_FALSE(header(got.headers, "x-missing").has_value());
    CHECK(got.peer.address == "127.0.0.1");
    CHECK(got.peer.port != 0);
    CHECK_FALSE(got.tls);
    CHECK_FALSE(got.body_too_large);
}

TEST_CASE("404 and 405 reach the fallback with path_known", "[platform]") {
    Fixture f;
    f.server->route("GET", "/known", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {}, "known");
    });
    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> calls;
    f.server->set_fallback(
        [&](const HttpRequest& request, bool path_known, std::shared_ptr<Responder> responder) {
            {
                std::lock_guard lock(mutex);
                calls.emplace_back(request.method + " " + request.path, path_known);
            }
            responder->send(path_known ? 405 : 404, {}, path_known ? "wrong method" : "nowhere");
        });
    const auto endpoint = f.listen();

    const auto missing = f.client->send(t::request("GET", t::url(endpoint.port, "/missing")), {});
    const auto wrong = f.client->send(t::request("DELETE", t::url(endpoint.port, "/known")), {});
    REQUIRE(missing);
    REQUIRE(wrong);
    CHECK(missing->status == 404);
    CHECK(missing->body == "nowhere");
    CHECK(wrong->status == 405);
    CHECK(wrong->body == "wrong method");
    std::lock_guard lock(mutex);
    CHECK(calls == std::vector<std::pair<std::string, bool>>{{"GET /missing", false},
                                                             {"DELETE /known", true}});
}

TEST_CASE("without a fallback, 404 and 405 answer as FastAPI does", "[platform]") {
    Fixture f;
    f.server->route("GET", "/known", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {}, "known");
    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    client.send("GET /missing HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto missing = client.read_response();
    CHECK(missing.starts_with("HTTP/1.1 404 Not Found\r\ndate: "));
    CHECK(missing.ends_with("\r\ncontent-length: 22\r\ncontent-type: application/json\r\n\r\n"
                            R"({"detail":"Not Found"})"));

    client.send("POST /known HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    const auto wrong = client.read_response();
    CHECK(wrong.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(wrong.find("\r\nallow: GET, HEAD\r\n") != std::string::npos);
    CHECK(wrong.ends_with(R"({"detail":"Method Not Allowed"})"));
}

TEST_CASE("HEAD runs the GET route and sends no body", "[platform]") {
    Fixture f;
    f.server->route("GET", "/page", [](const HttpRequest& request, std::shared_ptr<Responder> r) {
        r->send(200, {{"x-method", request.method}}, "hello");
    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    client.send("HEAD /page HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto head = client.read_response(true);
    CHECK(head.find("\r\nx-method: HEAD\r\n") != std::string::npos);
    CHECK(head.find("\r\ncontent-length: 5\r\n") != std::string::npos);
    // The connection stays usable: the next answer starts right after the head.
    client.send("GET /page HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(client.read_response().ends_with("\r\n\r\nhello"));
}

TEST_CASE("a path that matches with its trailing slash toggled redirects", "[platform]") {
    Fixture f;
    f.server->route("GET", "/v1/filters", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {}, "ok");
    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    client.send("GET /v1/filters/?a=1 HTTP/1.1\r\nHost: localhost:9\r\n\r\n");
    const auto response = client.read_response();
    CHECK(response.starts_with("HTTP/1.1 307 Temporary Redirect\r\n"));
    CHECK(
        response.find("\r\ncontent-length: 0\r\nlocation: http://localhost:9/v1/filters?a=1\r\n") !=
        std::string::npos);
}

TEST_CASE("handler header names go out in lower case after the date", "[platform]") {
    Fixture f;
    f.server->route("GET", "/", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(201, {{"Content-Type", "application/json"}, {"X-Request-ID", "abc"}}, "{}");
    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto response = client.read_response();
    const auto date_end = response.find("\r\n", response.find("\r\ndate: ") + 2);
    CHECK(response.starts_with("HTTP/1.1 201 Created\r\ndate: "));
    CHECK(response.substr(date_end) ==
          "\r\ncontent-type: application/json\r\nx-request-id: abc\r\ncontent-length: 2\r\n\r\n{}");
}

TEST_CASE("a declared Content-Length over the cap reaches the handler unread", "[platform]") {
    Fixture f;
    std::promise<HttpRequest> seen;
    f.server->route("POST", "/upload", RouteOptions{.max_body = 1024},
                    [&](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        seen.set_value(request);
                        responder->send(413, {}, "too large");
                    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    // Only the head goes out: the answer must come without the body ever being sent.
    client.send("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5000000\r\n\r\n");
    const auto response = client.read_response();
    CHECK(response.starts_with("HTTP/1.1 413 Request Entity Too Large\r\n"));
    CHECK(response.find("\r\nconnection: close\r\n") != std::string::npos);
    const HttpRequest got = seen.get_future().get();
    CHECK(got.body_too_large);
    CHECK(got.body.empty());
    CHECK(client.closed_by_server());
}

TEST_CASE("a chunked body over the cap reaches the handler before it is buffered", "[platform]") {
    Fixture f;
    std::promise<HttpRequest> seen;
    f.server->route("POST", "/upload", RouteOptions{.max_body = 1024},
                    [&](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        seen.set_value(request);
                        responder->send(413, {}, "too large");
                    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    client.send("POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    const std::string chunk(800, 'a');
    // Two chunks pass the cap; the rest of the body is never sent before the answer arrives.
    client.send("320\r\n" + chunk + "\r\n320\r\n" + chunk + "\r\n");
    const auto response = client.read_response();
    CHECK(response.starts_with("HTTP/1.1 413 "));
    const HttpRequest got = seen.get_future().get();
    CHECK(got.body_too_large);
    CHECK(got.body.empty());
}

TEST_CASE("a body within the cap arrives whole, through curl's 100-continue", "[platform]") {
    Fixture f;
    f.server->route("POST", "/upload", RouteOptions{.max_body = 4 << 20},
                    [](const HttpRequest& request, std::shared_ptr<Responder> responder) {
                        responder->send(200, {}, std::to_string(request.body.size()));
                    });
    const auto endpoint = f.listen();
    auto request = t::request("POST", t::url(endpoint.port, "/upload"), std::string(2 << 20, 'x'));
    request.headers = {{"Expect", "100-continue"}};
    const auto response = f.client->send(request, {});
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->body == std::to_string(2 << 20));
}

TEST_CASE("a handler may answer later from another thread", "[platform]") {
    Fixture f;
    ThreadPool pool("fv-test-pool", 2);
    std::shared_ptr<Responder> parked;
    bool ran_on_pool = false;
    std::mutex mutex;
    std::condition_variable arrived;
    f.server->route("GET", "/later", RouteOptions{.run_on = &pool},
                    [&](const HttpRequest&, std::shared_ptr<Responder> responder) {
                        std::lock_guard lock(mutex);
                        ran_on_pool = pool.is_current();
                        parked = std::move(responder);
                        arrived.notify_all();
                    });
    const auto endpoint = f.listen();

    auto answer = std::async(std::launch::async, [&] {
        return f.client->send(t::request("GET", t::url(endpoint.port, "/later")), {});
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(arrived.wait_for(lock, 10s, [&] { return parked != nullptr; }));
        CHECK(ran_on_pool);
    }
    std::thread([responder = std::move(parked)] {
        std::this_thread::sleep_for(50ms);
        responder->send(200, {}, "done later");
        responder->send(500, {}, "ignored"); // a second answer is dropped
    }).join();
    const auto response = answer.get();
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->body == "done later");
    pool.shutdown(1s);
}

TEST_CASE("a handler that drops its responder answers 500", "[platform]") {
    Fixture f;
    f.server->route("GET", "/forgot", [](const HttpRequest&, std::shared_ptr<Responder>) {});
    const auto endpoint = f.listen();
    const auto response = f.client->send(t::request("GET", t::url(endpoint.port, "/forgot")), {});
    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(response->body == "Internal Server Error");
}

TEST_CASE("keep-alive serves several requests on one connection", "[platform]") {
    Fixture f;
    std::atomic<int> count{0};
    f.server->route("GET", "/n", [&](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {}, std::to_string(++count));
    });
    const auto endpoint = f.listen();

    t::RawClient client(endpoint.port);
    for (int i = 1; i <= 3; ++i) {
        client.send("GET /n HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto response = client.read_response();
        CHECK(response.find("connection: close") == std::string::npos);
        CHECK(response.ends_with("\r\n\r\n" + std::to_string(i)));
    }
    client.send("GET /n HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(client.read_response().find("\r\nconnection: close\r\n") != std::string::npos);
    CHECK(client.closed_by_server());
}

TEST_CASE("HTTP/1.0 requests close after the answer", "[platform]") {
    Fixture f;
    f.server->route("GET", "/", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {}, "old");
    });
    const auto endpoint = f.listen();
    t::RawClient client(endpoint.port);
    client.send("GET / HTTP/1.0\r\n\r\n");
    const auto response = client.read_to_end();
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.find("\r\nconnection: close\r\n") != std::string::npos);
    CHECK(response.ends_with("\r\n\r\nold"));
}

TEST_CASE("a malformed request is answered 400 and closed", "[platform]") {
    Fixture f;
    const auto endpoint = f.listen();
    t::ScopedCapture capture;
    t::RawClient client(endpoint.port);
    client.send("NOT A REQUEST\r\n\r\n");
    const auto response = client.read_to_end();
    CHECK(response.starts_with("HTTP/1.1 400 Bad Request\r\ndate: "));
    CHECK(response.ends_with("\r\ncontent-type: text/plain; charset=utf-8\r\ncontent-length: 30"
                             "\r\nconnection: close\r\n\r\nInvalid HTTP request received."));
    bool warned = false;
    for (const auto& line : capture->lines()) {
        warned = warned || (line.level == log::Level::Warning && line.logger == "uvicorn.error" &&
                            line.message == "Invalid HTTP request received.");
    }
    CHECK(warned);
}

TEST_CASE("50 concurrent keep-alive requests are all answered", "[platform]") {
    Fixture f;
    ThreadPool pool("fv-test-pool", 4);
    f.server->route("GET", "/item/{n}", RouteOptions{.run_on = &pool},
                    [](const HttpRequest& request, std::shared_ptr<Responder> r) {
                        std::this_thread::sleep_for(5ms);
                        r->send(200, {}, "item " + param(request, "n"));
                    });
    const auto endpoint = f.listen();

    std::vector<std::future<bool>> results;
    for (int worker = 0; worker < 50; ++worker) {
        results.push_back(std::async(std::launch::async, [&, worker] {
            t::RawClient client(endpoint.port);
            for (int round = 0; round < 4; ++round) {
                const std::string n = std::to_string(worker * 10 + round);
                client.send("GET /item/" + n + " HTTP/1.1\r\nHost: x\r\n\r\n");
                if (!client.read_response().ends_with("\r\n\r\nitem " + n)) {
                    return false;
                }
            }
            return true;
        }));
    }
    int answered = 0;
    for (auto& result : results) {
        answered += result.get() ? 1 : 0;
    }
    CHECK(answered == 50);

    // And the same through curl, whose connection cache reuses keep-alive connections.
    std::vector<std::future<int>> statuses;
    for (int worker = 0; worker < 50; ++worker) {
        statuses.push_back(std::async(std::launch::async, [&, worker] {
            const auto response = f.client->send(
                t::request("GET", t::url(endpoint.port, "/item/" + std::to_string(worker))), {});
            return response ? response->status : 0;
        }));
    }
    int ok = 0;
    for (auto& status : statuses) {
        ok += status.get() == 200 ? 1 : 0;
    }
    CHECK(ok == 50);
    f.server->stop(1s);
    pool.shutdown(1s);
}

TEST_CASE("route patterns Starlette refuses are refused", "[platform]") {
    Fixture f;
    const auto handler = [](const HttpRequest&, std::shared_ptr<Responder>) {
    };
    CHECK_THROWS_AS(f.server->route("GET", "/a/{x:uuid4}", handler), std::invalid_argument);
    CHECK_THROWS_AS(f.server->route("GET", "/a/{x}/{x}", handler), std::invalid_argument);
}

TEST_CASE("a TLS listener without a certificate fails listen with the reason", "[platform]") {
    Fixture f;
    const auto result =
        f.server->listen(Listen{.address = "127.0.0.1", .port = 0, .tls = TlsIdentity{}});
    REQUIRE_FALSE(result);
    CHECK(result.error().starts_with("Could not listen on 127.0.0.1:0: the TLS certificate"));
}

TEST_CASE("stop does not wait for a handler that has not answered", "[platform]") {
    Fixture f;
    std::shared_ptr<Responder> parked;
    std::mutex mutex;
    std::condition_variable arrived;
    f.server->route("GET", "/stuck", [&](const HttpRequest&, std::shared_ptr<Responder> r) {
        std::lock_guard lock(mutex);
        parked = std::move(r); // stands in for a handler waiting on the GUI thread
        arrived.notify_all();
    });
    const auto endpoint = f.listen();
    auto answer = std::async(std::launch::async, [&] {
        return f.client->send(t::request("GET", t::url(endpoint.port, "/stuck")), {});
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(arrived.wait_for(lock, 10s, [&] { return parked != nullptr; }));
    }
    const auto started = std::chrono::steady_clock::now();
    f.server->stop(200ms);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(answer.wait_for(10s) == std::future_status::ready);
    CHECK_FALSE(answer.get().has_value()); // the connection closed without an answer
    std::lock_guard lock(mutex);
    parked->send(200, {}, "too late"); // dropped: the connection is gone
    parked.reset();
}