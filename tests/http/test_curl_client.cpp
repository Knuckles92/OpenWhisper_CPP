// The curl client against servers on the loopback interface (loopback_server.h). No test here
// reaches the internet: proxies are set per client, and the one request to a made-up host name
// is pinned to 127.0.0.1.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "base/log.h"
#include "http/client.h"
#include "http/user_agent.h"
#include "loopback_server.h"
#include "testkit/testkit.h"

using fv::testing::LoopbackServer;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<fv::http::Client> direct_client() {
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return fv::http::make_curl_client(options);
}

fv::http::Request get(std::string url) {
    fv::http::Request request;
    request.url = std::move(url);
    return request;
}

LoopbackServer::Handler reply_with(std::string raw) {
    return [raw](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(raw);
    };
}

class CaptureSink final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        records.push_back({record.level, std::string(record.logger), std::string(record.message)});
    }
    struct Line {
        fv::log::Level level;
        std::string logger;
        std::string message;
    };
    // The records of the HTTP loggers only.
    std::vector<Line> lines() {
        std::lock_guard lock(mutex_);
        std::vector<Line> http;
        for (const auto& line : records) {
            if (line.logger.starts_with("httpx") || line.logger.starts_with("httpcore")) {
                http.push_back(line);
            }
        }
        return http;
    }

private:
    std::mutex mutex_;
    std::vector<Line> records;
};

// Everything written to the process's stderr while it lives, read back on finish().
class StderrCapture {
public:
    explicit StderrCapture(std::filesystem::path file) : file_(std::move(file)) {
        std::fflush(stderr);
#ifdef _WIN32
        saved_ = _dup(2);
        FILE* sink = nullptr;
        _wfopen_s(&sink, file_.c_str(), L"wb");
        _dup2(_fileno(sink), 2);
        std::fclose(sink);
#else
        saved_ = dup(2);
        FILE* sink = std::fopen(file_.c_str(), "wb");
        dup2(fileno(sink), 2);
        std::fclose(sink);
#endif
    }
    std::string finish() {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved_, 2);
        _close(saved_);
#else
        dup2(saved_, 2);
        close(saved_);
#endif
        std::ifstream in(file_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

private:
    std::filesystem::path file_;
    int saved_ = -1;
};

class BufferSink final : public fv::http::BodySink {
public:
    bool stop_at_head = false;
    std::size_t stop_after = std::string::npos;
    std::optional<fv::http::Response> head;
    std::string body;

    bool on_head(const fv::http::Response& response) override {
        head = response;
        return !stop_at_head;
    }
    bool write(std::span<const char> data) override {
        body.append(data.data(), data.size());
        return body.size() < stop_after;
    }
};

} // namespace

// from tests/test_automation_fix_pipeline_sources.py::test_http_client_loggers_only_warn
TEST_CASE("test_http_client_loggers_only_warn", "[test_automation_fix_pipeline_sources]") {
    // Adapted: Python checks that setup_logging holds httpx and httpcore at WARNING. Here the
    // client logs under those names, and a request made under that hold (as install_app_logging
    // sets it) puts nothing below WARNING in the log and nothing on stderr (curl's verbose
    // output stays off).
    LoopbackServer server(reply_with(LoopbackServer::response(200, "OK", {}, "fine")));
    auto capture = std::make_shared<CaptureSink>();
    fv::log::add_sink(capture);
    const fv::testkit::TempDataRoot root;

    // Without the hold, the request is logged at INFO, so the check below can fail.
    fv::log::set_logger_level("httpx", std::nullopt);
    fv::log::set_logger_level("httpcore", std::nullopt);
    REQUIRE(direct_client()->send(get(server.url("/unheld?token=secret")), {}).has_value());
    const auto unheld = capture->lines();
    REQUIRE(unheld.size() == 1);
    CHECK(unheld[0].logger == "httpx");
    CHECK(unheld[0].level == fv::log::Level::Info);
    CHECK(unheld[0].message ==
          "HTTP Request: GET " + server.url("/unheld") + " \"HTTP/1.1 200 OK\"");

    fv::log::set_logger_level("httpx", fv::log::Level::Warning);
    fv::log::set_logger_level("httpcore", fv::log::Level::Warning);
    StderrCapture stderr_capture(root.path() / "stderr.txt");
    const auto response = direct_client()->send(get(server.url("/held")), {});
    const auto written = stderr_capture.finish();
    fv::log::remove_sink(capture);
    fv::log::set_logger_level("httpx", std::nullopt);
    fv::log::set_logger_level("httpcore", std::nullopt);

    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    for (const auto& line : capture->lines()) {
        CHECK((line.level >= fv::log::Level::Warning ||
               line.message.find("/unheld") != std::string::npos));
    }
    CHECK(capture->lines().size() == unheld.size());
    CHECK(written.empty());
}

TEST_CASE("a request that asks not to be logged leaves no request line", "[curl_client]") {
    // webhooks.py _QuietRequests: with httpx at INFO, a quiet request logs nothing.
    LoopbackServer server(reply_with(LoopbackServer::response(200, "OK", {}, "fine")));
    auto capture = std::make_shared<CaptureSink>();
    fv::log::add_sink(capture);
    fv::log::set_logger_level("httpx", std::nullopt);
    auto quiet = get(server.url("/quiet"));
    quiet.log_request = false;
    const auto response = direct_client()->send(quiet, {});
    fv::log::remove_sink(capture);

    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(capture->lines().empty());
}

TEST_CASE("a response comes back with its status line, headers and body", "[curl_client]") {
    LoopbackServer server(reply_with(LoopbackServer::response(
        404, "Not Found", {{"X-Thing", "  padded value"}, {"x-thing", "second"}}, "missing")));
    auto request = get(server.url("/path?q=1"));
    request.headers = {{"User-Agent", fv::http::user_agent()}, {"X-Empty", ""}};
    const auto response = direct_client()->send(request, {});
    REQUIRE(response.has_value());
    CHECK(response->status == 404);
    CHECK(response->reason == "Not Found");
    CHECK(response->body == "missing");
    CHECK(response->effective_url == server.url("/path?q=1"));
    CHECK(fv::http::find_header(response->headers, "X-THING") == "padded value");
    CHECK(fv::http::find_header(response->headers, "content-length") == "7");
    CHECK_FALSE(fv::http::find_header(response->headers, "X-Missing"));

    const auto seen = server.requests();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].method == "GET");
    CHECK(seen[0].target == "/path?q=1");
    CHECK(seen[0].header("User-Agent") == fv::http::user_agent());
    CHECK(seen[0].header("X-Empty") == "");
    CHECK_FALSE(seen[0].header("Range"));
    CHECK(fv::http::user_agent().starts_with("OpenWhisper_CPP/"));
}

TEST_CASE("range_from asks for the rest of the file", "[curl_client]") {
    LoopbackServer server(reply_with(LoopbackServer::response(
        206, "Partial Content", {{"Content-Range", "bytes 8-16/17"}}, "-payload")));
    auto request = get(server.url("/file"));
    request.range_from = 8;
    const auto response = direct_client()->send(request, {});
    REQUIRE(response.has_value());
    CHECK(response->status == 206);
    CHECK(server.requests().at(0).header("Range") == "bytes=8-");
}

TEST_CASE("redirects are followed only when asked", "[curl_client]") {
    LoopbackServer target(reply_with(LoopbackServer::response(200, "OK", {}, "moved here")));
    LoopbackServer server(reply_with(
        LoopbackServer::response(302, "Found", {{"Location", target.url("/final")}}, "")));

    const auto client = direct_client();
    const auto unfollowed = client->send(get(server.url("/start")), {});
    REQUIRE(unfollowed.has_value());
    CHECK(unfollowed->status == 302);
    CHECK(fv::http::find_header(unfollowed->headers, "Location") == target.url("/final"));

    auto request = get(server.url("/start"));
    request.follow_redirects = true;
    request.range_from = 3;
    const auto followed = client->send(request, {});
    REQUIRE(followed.has_value());
    CHECK(followed->status == 200);
    CHECK(followed->body == "moved here");
    CHECK(followed->effective_url == target.url("/final"));
    // The headers of the final response only; urllib keeps Range on the redirected request.
    CHECK_FALSE(fv::http::find_header(followed->headers, "Location"));
    CHECK(target.requests().at(0).header("Range") == "bytes=3-");
}

TEST_CASE("a body sink receives the head, then the body, and may stop either", "[curl_client]") {
    const std::string body(200'000, 'b');
    LoopbackServer server(reply_with(LoopbackServer::response(200, "OK", {}, body)));
    const auto client = direct_client();

    BufferSink whole;
    const auto streamed = client->send(get(server.url("/a")), {}, &whole);
    REQUIRE(streamed.has_value());
    REQUIRE(whole.head);
    CHECK(whole.head->status == 200);
    CHECK(whole.head->effective_url == server.url("/a"));
    CHECK(whole.body == body);
    CHECK(streamed->body.empty());

    BufferSink refusing;
    refusing.stop_at_head = true;
    const auto refused = client->send(get(server.url("/b")), {}, &refusing);
    REQUIRE(refused.has_value());
    CHECK(refused->status == 200);
    CHECK(refusing.body.empty());

    BufferSink partial;
    partial.stop_after = 1;
    const auto stopped = client->send(get(server.url("/c")), {}, &partial);
    REQUIRE(stopped.has_value());
    CHECK(stopped->status == 200);
    CHECK(partial.body.size() < body.size());
}

TEST_CASE("an empty body still reaches the sink's head", "[curl_client]") {
    LoopbackServer server(reply_with(LoopbackServer::response(204, "No Content", {}, "")));
    BufferSink sink;
    const auto response = direct_client()->send(get(server.url("/")), {}, &sink);
    REQUIRE(response.has_value());
    REQUIRE(sink.head);
    CHECK(sink.head->status == 204);
}

TEST_CASE("a body over max_body_bytes is refused", "[curl_client]") {
    LoopbackServer server(
        reply_with(LoopbackServer::response(200, "OK", {}, std::string(5000, 'x'))));
    auto request = get(server.url("/"));
    request.max_body_bytes = 4096;
    const auto response = direct_client()->send(request, {});
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::TooLarge);
}

TEST_CASE("cancel stops a stalled transfer", "[curl_client]") {
    LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc");
        reply.wait(30s);
    });
    fv::CancelSource cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(300ms);
        cancel.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto response = direct_client()->send(get(server.url("/")), cancel.token());
    canceller.join();
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Cancelled);
    CHECK(std::chrono::steady_clock::now() - started < 10s);

    fv::CancelSource already;
    already.cancel();
    const auto refused = direct_client()->send(get(server.url("/")), already.token());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == fv::http::ErrorKind::Cancelled);
}

TEST_CASE("a server that stops sending times out with the head read", "[curl_client]") {
    LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc");
        reply.wait(30s);
    });
    auto request = get(server.url("/"));
    request.idle_timeout = 1s;
    BufferSink sink;
    const auto response = direct_client()->send(request, {}, &sink);
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Timeout);
    CHECK(response.error().message == "timed out");
    CHECK(response.error().after_head);
    CHECK(sink.body == "abc");
}

TEST_CASE("a connection closed before the declared length is a truncated body", "[curl_client]") {
    LoopbackServer server(reply_with("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort"));
    BufferSink sink;
    const auto response = direct_client()->send(get(server.url("/")), {}, &sink);
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().truncated);
    CHECK(response.error().after_head);
    CHECK(sink.body == "short");
}

TEST_CASE("a past deadline fails before connecting", "[curl_client]") {
    auto request = get("http://127.0.0.1:9/");
    request.deadline = std::chrono::steady_clock::now() - 1s;
    const auto response = direct_client()->send(request, {});
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Timeout);
}

TEST_CASE("a refused connection is a connect error worded as Python's", "[curl_client]") {
    std::string url;
    {
        LoopbackServer closed(reply_with(""));
        url = closed.url("/");
    }
    const auto response = direct_client()->send(get(url), {});
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Connect);
    CHECK_FALSE(response.error().after_head);
    CHECK_FALSE(response.error().after_request);
#ifdef _WIN32
    CHECK(response.error().message ==
          "[WinError 10061] No connection could be made because the target machine actively "
          "refused it");
#endif
}

TEST_CASE("a failure once the request was sent is told apart from one before it", "[curl_client]") {
    // urllib wraps only connect, handshake and send failures in a URLError; a server that takes
    // the request and then stalls or hangs up reaches the caller as the bare socket error.
    LoopbackServer silent(
        [](const LoopbackServer::Request&, LoopbackServer::Reply& reply) { reply.wait(30s); });
    auto request = get(silent.url("/"));
    request.idle_timeout = 1s;
    const auto stalled = direct_client()->send(request, {});
    REQUIRE_FALSE(stalled.has_value());
    CHECK(stalled.error().kind == fv::http::ErrorKind::Timeout);
    CHECK(stalled.error().message == "timed out");
    CHECK(stalled.error().after_request);
    CHECK_FALSE(stalled.error().after_head);

    LoopbackServer hangup(reply_with(""));
    const auto closed = direct_client()->send(get(hangup.url("/")), {});
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().message == "Remote end closed connection without response");
    CHECK(closed.error().after_request);
    CHECK_FALSE(closed.error().after_head);

    // A followed redirect opens the next hop as a new request: failing to reach it comes before
    // that request again.
    std::string gone;
    {
        LoopbackServer closed_server(reply_with(""));
        gone = closed_server.url("/gone");
    }
    LoopbackServer redirect(
        reply_with(LoopbackServer::response(302, "Found", {{"Location", gone}}, "")));
    auto followed = get(redirect.url("/start"));
    followed.follow_redirects = true;
    const auto hop = direct_client()->send(followed, {});
    REQUIRE_FALSE(hop.has_value());
    CHECK(hop.error().kind == fv::http::ErrorKind::Connect);
    CHECK_FALSE(hop.error().after_request);
    CHECK_FALSE(hop.error().after_head);
}

TEST_CASE("request bodies are sent as bytes or from a file", "[curl_client]") {
    LoopbackServer server(reply_with(LoopbackServer::response(200, "OK", {}, "")));
    const auto client = direct_client();
    auto request = get(server.url("/post"));
    request.method = "POST";
    request.body = std::string("payload bytes");
    REQUIRE(client->send(request, {}).has_value());

    const fv::testkit::TempDataRoot root;
    const auto file = root.path() / "body.bin";
    std::ofstream(file, std::ios::binary) << std::string(70'000, 'f');
    request.method = "PUT";
    request.body = file;
    REQUIRE(client->send(request, {}).has_value());

    const auto seen = server.requests();
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].method == "POST");
    CHECK(seen[0].body == "payload bytes");
    CHECK(seen[1].method == "PUT");
    CHECK(seen[1].body == std::string(70'000, 'f'));
    CHECK_FALSE(seen[1].header("Expect"));

    // A multipart file part that cannot be opened fails before anything is sent.
    request.body = fv::http::Multipart{{{"file", "gone.wav", "audio/wav", root.path() / "gone"}}};
    const auto multipart = client->send(request, {});
    REQUIRE_FALSE(multipart.has_value());
    CHECK(multipart.error().kind == fv::http::ErrorKind::Protocol);
    CHECK(server.requests().size() == 2);
}

TEST_CASE("a pinned address connects there and keeps the host name", "[curl_client]") {
    LoopbackServer server(reply_with(LoopbackServer::response(200, "OK", {}, "pinned")));
    auto request =
        get("http://openwhisper-cpp-test.invalid:" + std::to_string(server.port()) + "/pinned");
    request.pinned_address = "127.0.0.1";
    const auto response = direct_client()->send(request, {});
    REQUIRE(response.has_value());
    CHECK(response->body == "pinned");
    CHECK(server.requests().at(0).header("Host") ==
          "openwhisper-cpp-test.invalid:" + std::to_string(server.port()));
}

TEST_CASE("requests go through the proxy the resolver names", "[curl_client]") {
    LoopbackServer proxy(reply_with(LoopbackServer::response(200, "OK", {}, "via proxy")));
    std::vector<std::string> asked;
    fv::http::CurlClientOptions options;
    options.proxy = [&](std::string_view url) {
        asked.emplace_back(url);
        return std::optional<std::string>(proxy.url(""));
    };
    const auto client = fv::http::make_curl_client(options);
    const auto response = client->send(get("http://openwhisper-cpp-test.invalid/through"), {});
    REQUIRE(response.has_value());
    CHECK(response->body == "via proxy");
    CHECK(asked == std::vector<std::string>{"http://openwhisper-cpp-test.invalid/through"});
    CHECK(proxy.requests().at(0).target == "http://openwhisper-cpp-test.invalid/through");

    // bypass_proxy skips the resolver altogether.
    LoopbackServer direct(reply_with(LoopbackServer::response(200, "OK", {}, "direct")));
    auto request = get(direct.url("/"));
    request.bypass_proxy = true;
    const auto bypassed = client->send(request, {});
    REQUIRE(bypassed.has_value());
    CHECK(bypassed->body == "direct");
    CHECK(asked.size() == 1);
}

TEST_CASE("the default client goes through the system proxy settings", "[curl_client]") {
    // HTTP_PROXY is the environment half of urllib's getproxies(); any environment entry takes
    // precedence over the registry, so this holds whatever the machine's Internet Settings say.
    // The target is a closed loopback port: only the proxy can answer.
    LoopbackServer proxy(reply_with(LoopbackServer::response(200, "OK", {}, "via system proxy")));
    std::string target;
    {
        LoopbackServer closed(reply_with(""));
        target = closed.url("/through");
    }
    // Lower-case names are distinct variables outside Windows, and win there.
    const std::vector<std::string> names = {"HTTP_PROXY", "http_proxy", "NO_PROXY", "no_proxy",
                                            "REQUEST_METHOD"};
    std::vector<std::optional<std::string>> saved;
    for (const auto& name : names) {
        saved.push_back(fv::testkit::get_env(name));
    }
    fv::testkit::set_env("HTTP_PROXY", proxy.url(""));
    fv::testkit::set_env("http_proxy", proxy.url(""));
    fv::testkit::set_env("NO_PROXY", std::nullopt);
    fv::testkit::set_env("no_proxy", std::nullopt);
    fv::testkit::set_env("REQUEST_METHOD", std::nullopt);
    const auto response = fv::http::make_curl_client()->send(get(target), {});
    for (std::size_t i = 0; i < names.size(); ++i) {
        fv::testkit::set_env(names[i], saved[i]);
    }
    REQUIRE(response.has_value());
    CHECK(response->body == "via system proxy");
    CHECK(proxy.requests().at(0).target == target);
}

TEST_CASE("an untrusted certificate is a certificate failure", "[curl_client]") {
    LoopbackServer server(reply_with(LoopbackServer::response(200, "OK", {}, "secret")), true);
    const auto response = direct_client()->send(get(server.url("/")), {});
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Tls);
    CHECK(response.error().certificate);
    CHECK_FALSE(response.error().after_head);
}
