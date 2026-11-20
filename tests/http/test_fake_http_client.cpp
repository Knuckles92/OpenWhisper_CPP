// The scripted client that download tests use in place of curl.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "http/testing/fake_http_client.h"

using fv::http::FakeHttpClient;
using fv::http::ScriptedResponse;

namespace {

class Collect final : public fv::http::BodySink {
public:
    std::string body;
    int pieces = 0;
    bool write(std::span<const char> data) override {
        body.append(data.data(), data.size());
        ++pieces;
        return true;
    }
};

} // namespace

TEST_CASE("serve answers Range as a static file server does", "[fake_http_client]") {
    fv::http::Request request;
    const auto whole = FakeHttpClient::serve(request, "component-payload");
    CHECK(whole.status == 200);
    CHECK(whole.body == "component-payload");
    CHECK(fv::http::find_header(whole.headers, "Content-Length") == "17");

    request.range_from = 8;
    const auto rest = FakeHttpClient::serve(request, "component-payload");
    CHECK(rest.status == 206);
    CHECK(rest.body == "t-payload");
    CHECK(fv::http::find_header(rest.headers, "Content-Range") == "bytes 8-16/17");
    CHECK(fv::http::find_header(rest.headers, "Content-Length") == "9");

    request.range_from = 17;
    const auto past = FakeHttpClient::serve(request, "component-payload");
    CHECK(past.status == 416);
    CHECK(fv::http::find_header(past.headers, "Content-Range") == "bytes */17");
}

TEST_CASE("scripted responses arrive in order, in pieces, then cut", "[fake_http_client]") {
    FakeHttpClient client;
    ScriptedResponse first;
    first.body = "abcdef";
    first.chunks = {2, 1};
    first.cut = fv::http::Error{fv::http::ErrorKind::Connect, "reset"};
    client.push(first);
    client.set_handler(
        [](const fv::http::Request& request) { return FakeHttpClient::serve(request, "handled"); });

    Collect sink;
    const auto cut = client.send(fv::http::Request{}, {}, &sink);
    REQUIRE_FALSE(cut.has_value());
    CHECK(cut.error().after_head);
    CHECK(cut.error().message == "reset");
    CHECK(sink.body == "abcdef");
    CHECK(sink.pieces == 3);

    const auto handled = client.send(fv::http::Request{}, {}, nullptr);
    REQUIRE(handled.has_value());
    CHECK(handled->body == "handled");
    CHECK(client.requests().size() == 2);
    CHECK(client.pending() == 0);

    fv::CancelSource cancel;
    cancel.cancel();
    const auto cancelled = client.send(fv::http::Request{}, cancel.token(), nullptr);
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().kind == fv::http::ErrorKind::Cancelled);
}
