// Server-Sent Events through HttpServer, read by the curl client of fv_http: the head goes out
// at once, every frame reaches the client before the next is written, a client that goes away
// is noticed within one write, and stop() ends open streams after its grace.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <expected>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "net/http_server.h"
#include "net/reactor.h"
#include "net_test_support.h"

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;

namespace {

// Collects the body as curl hands it over, one write() per piece curl received.
class FrameSink final : public http::BodySink {
public:
    bool on_head(const http::Response& head) override {
        std::lock_guard lock(mutex_);
        head_ = head;
        changed_.notify_all();
        return true;
    }
    bool write(std::span<const char> data) override {
        std::lock_guard lock(mutex_);
        reads_.emplace_back(data.begin(), data.end());
        received_.append(data.begin(), data.end());
        changed_.notify_all();
        return !hang_up_after_first_ || received_.empty();
    }

    void hang_up_after_first_write() { hang_up_after_first_ = true; }
    bool wait_for_bytes(std::size_t total, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return received_.size() >= total; });
    }
    std::string received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }
    std::vector<std::string> reads() const {
        std::lock_guard lock(mutex_);
        return reads_;
    }
    std::optional<http::Response> head() const {
        std::lock_guard lock(mutex_);
        return head_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<http::Response> head_;
    std::string received_;
    std::vector<std::string> reads_;
    bool hang_up_after_first_ = false;
};

// A server whose /events route hands its stream to the test.
struct SseFixture {
    Reactor reactor;
    std::unique_ptr<HttpServer> server = HttpServer::create(reactor);
    std::mutex mutex;
    std::condition_variable opened;
    std::shared_ptr<EventStream> stream;
    Endpoint endpoint;

    SseFixture() {
        server->route(
            "GET", "/v1/events", [this](const HttpRequest&, std::shared_ptr<Responder> responder) {
                auto events =
                    responder->start_sse({{"content-type", "text/event-stream; charset=utf-8"},
                                          {"cache-control", "no-cache"}});
                std::lock_guard lock(mutex);
                stream = std::move(events);
                opened.notify_all();
            });
        endpoint = t::listen_loopback(*server);
    }
    ~SseFixture() { server->stop(0ms); }

    std::shared_ptr<EventStream> wait_for_stream() {
        std::unique_lock lock(mutex);
        opened.wait_for(lock, 10s, [&] { return stream != nullptr; });
        return stream;
    }

    // The curl request, run on its own thread until the stream ends or the sink stops it.
    std::future<std::expected<http::Response, http::Error>> open(FrameSink& sink) {
        return std::async(std::launch::async, [this, &sink] {
            auto request = t::request("GET", t::url(endpoint.port, "/v1/events"));
            request.idle_timeout = 30s;
            return t::loopback_client()->send(request, CancelToken(), &sink);
        });
    }
};

std::string frame(int i) {
    return "id: " + std::to_string(i) +
           "\nevent: transcription.completed\ndata: {\"n\":" + std::to_string(i) + "}\n\n";
}

} // namespace

TEST_CASE("SSE sends its head at once and frames one by one", "[platform]") {
    SseFixture f;
    t::RawClient client(f.endpoint.port);
    client.send("GET /v1/events HTTP/1.1\r\nHost: x\r\n\r\n");
    auto stream = f.wait_for_stream();
    REQUIRE(stream);
    // Before any frame: the head alone has gone out.
    const auto head = client.read_until("\r\n\r\n");
    CHECK(head.starts_with("HTTP/1.1 200 OK\r\ndate: "));
    CHECK(head.ends_with("\r\ncontent-type: text/event-stream; charset=utf-8\r\n"
                         "cache-control: no-cache\r\ntransfer-encoding: chunked\r\n\r\n"));

    for (const std::string& text :
         {std::string("retry: 2000\n\n"), frame(1), std::string(": ping\n\n")}) {
        CHECK(stream->write(text));
        // One chunk per frame, lower-case hex size as uvicorn writes it.
        std::string size(16, '\0');
        size.resize(
            static_cast<std::size_t>(std::snprintf(size.data(), size.size(), "%zx", text.size())));
        CHECK(client.read_exactly(size.size() + 2 + text.size() + 2) ==
              size + "\r\n" + text + "\r\n");
    }
    CHECK(stream->write("")); // nothing is sent for an empty frame; the stream stays open
    stream->close();
    CHECK_FALSE(stream->write(frame(2))); // closed by the handler
    CHECK(client.read_to_end() == "0\r\n\r\n");
}

TEST_CASE("SSE frames reach curl as they are written", "[platform]") {
    SseFixture f;
    FrameSink sink;
    auto done = f.open(sink);
    auto stream = f.wait_for_stream();
    REQUIRE(stream);
    std::size_t total = 0;
    for (int i = 0; i < 3; ++i) {
        total += frame(i).size();
        CHECK(stream->write(frame(i)));
        REQUIRE(sink.wait_for_bytes(total, 10s));
    }
    const auto head = *sink.head();
    CHECK(head.status == 200);
    CHECK(http::find_header(head.headers, "content-type") == "text/event-stream; charset=utf-8");
    stream->close();
    const auto response = done.get();
    REQUIRE(response);
    CHECK(sink.received() == frame(0) + frame(1) + frame(2));
}

TEST_CASE("a 1,000-frame stream arrives in order with no frame merged into another", "[platform]") {
    SseFixture f;
    FrameSink sink;
    auto done = f.open(sink);
    auto stream = f.wait_for_stream();
    REQUIRE(stream);

    std::vector<std::size_t> boundaries{0};
    std::string expected;
    for (int i = 0; i < 1000; ++i) {
        const std::string text = frame(i);
        expected += text;
        boundaries.push_back(expected.size());
        REQUIRE(stream->write(text));
        // The next frame is written only once this one has reached the client: a server that
        // held frames back (Nagle, a buffer waiting to fill) would stall here.
        REQUIRE(sink.wait_for_bytes(expected.size(), 10s));
    }
    stream->close();
    REQUIRE(done.get());
    CHECK(sink.received() == expected);

    // No single read of the client spans two frames.
    std::size_t offset = 0;
    std::size_t next = 1;
    for (const auto& read : sink.reads()) {
        while (boundaries[next] <= offset) {
            ++next;
        }
        CHECK(offset + read.size() <= boundaries[next]);
        offset += read.size();
    }
}

TEST_CASE("a client that disconnects makes write return false within one write", "[platform]") {
    SseFixture f;
    FrameSink sink;
    sink.hang_up_after_first_write();
    auto done = f.open(sink);
    auto stream = f.wait_for_stream();
    REQUIRE(stream);
    CHECK(stream->write(frame(1)));
    (void)done.get(); // curl stopped reading and dropped the connection
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(stream->write(frame(2)));
    CHECK_FALSE(stream->write(frame(3)));
}

TEST_CASE("stop ends an open SSE stream after the grace and releases the port", "[platform]") {
    SseFixture f;
    FrameSink sink;
    auto done = f.open(sink);
    auto stream = f.wait_for_stream();
    REQUIRE(stream);
    REQUIRE(stream->write(frame(1)));
    REQUIRE(sink.wait_for_bytes(frame(1).size(), 10s));

    const auto started = std::chrono::steady_clock::now();
    f.server->stop(1s);
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(took >= 900ms);
    CHECK(took < 5s);
    REQUIRE(done.wait_for(10s) == std::future_status::ready);
    (void)done.get();
    CHECK_FALSE(stream->write(frame(2)));

    Reactor other;
    auto second = HttpServer::create(other);
    const auto rebound = second->listen(Listen{.address = "127.0.0.1", .port = f.endpoint.port});
    CHECK(rebound.has_value());
    second->stop(0ms);
}

TEST_CASE("a stream dropped without close ends as close ends it", "[platform]") {
    SseFixture f;
    t::RawClient client(f.endpoint.port);
    client.send("GET /v1/events HTTP/1.1\r\nHost: x\r\n\r\n");
    auto stream = f.wait_for_stream();
    REQUIRE(stream);
    REQUIRE(client.read_until("\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(stream->write(": ping\n\n"));
    {
        std::lock_guard lock(f.mutex);
        f.stream.reset();
    }
    stream.reset(); // the last owner lets go: the stream ends, the connection closes
    CHECK(client.read_to_end() == "8\r\n: ping\n\n\r\n0\r\n\r\n");
}

TEST_CASE("a second start_sse is refused and dropping it leaves the first open", "[platform]") {
    SseFixture f;
    std::shared_ptr<EventStream> first;
    std::promise<bool> second_refused;
    f.server->route("GET", "/twice", [&](const HttpRequest&, std::shared_ptr<Responder> r) {
        first = r->start_sse({{"content-type", "text/event-stream"}});
        auto second = r->start_sse({});
        second_refused.set_value(!second->write("data: no\n\n"));
    }); // the second stream is dropped here
    t::RawClient client(f.endpoint.port);
    client.send("GET /twice HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(second_refused.get_future().get());
    REQUIRE(client.read_until("\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(first->write("data: yes\n\n"));
    CHECK(client.read_exactly(16) == "b\r\ndata: yes\n\n\r\n");
    first->close();
    CHECK(client.read_to_end() == "0\r\n\r\n");
}

TEST_CASE("frames a slow client has not taken show in queued_frames", "[platform]") {
    SseFixture f;
    t::RawClient client(f.endpoint.port);
    client.send("GET /v1/events HTTP/1.1\r\nHost: x\r\n\r\n");
    auto stream = f.wait_for_stream();
    REQUIRE(stream);
    REQUIRE_FALSE(client.read_until("\r\n\r\n").empty());
    CHECK(stream->queued_frames() == 0);

    // The client reads nothing: far more than the socket buffers can hold stays queued.
    const std::string data(64 * 1024 - 8, 'x');
    const std::string text = "data: " + data + "\n\n";
    constexpr int kFrames = 400;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(stream->write(text));
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (stream->queued_frames() == static_cast<std::size_t>(kFrames) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms); // the first frames go to the socket
    }
    const std::size_t stuck = stream->queued_frames();
    CHECK(stuck > 0);
    CHECK(stuck < static_cast<std::size_t>(kFrames));

    // Once the client reads, the queue empties.
    char size[16];
    const auto size_length =
        static_cast<std::size_t>(std::snprintf(size, sizeof(size), "%zx", text.size()));
    const std::size_t chunk = size_length + 2 + text.size() + 2;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(client.read_exactly(chunk).size() == chunk);
    }
    const auto drained = std::chrono::steady_clock::now() + 10s;
    while (stream->queued_frames() != 0 && std::chrono::steady_clock::now() < drained) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(stream->queued_frames() == 0);
    stream->close();
    CHECK(client.read_to_end() == "0\r\n\r\n");
}

TEST_CASE("stop closes idle keep-alive connections at once", "[platform]") {
    SseFixture f;
    f.server->route("GET", "/", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {}, "ok");
    });
    t::RawClient client(f.endpoint.port);
    client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_FALSE(client.read_response().empty());
    const auto started = std::chrono::steady_clock::now();
    f.server->stop(5s);
    CHECK(std::chrono::steady_clock::now() - started < 3s);
    CHECK(client.closed_by_server());
}
