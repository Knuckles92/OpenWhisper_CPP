// Responder::send_file as Starlette 1.6's FileResponse and StaticFiles answer: whole files,
// single and multiple ranges, 416 and 400, If-Range, the conditional 304, HEAD, and a large file
// streamed as the socket takes it. The pure planning (detail/file_response.h) is checked first,
// then the same answers over a loopback socket.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/detail/file_response.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "net_test_support.h"
#include "testkit/testkit.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;
namespace d = fv::net::detail;
using Kind = d::RangeRequest::Kind;
using Pairs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

namespace {

d::FileFacts facts(std::uint64_t size) {
    // 2024-11-15 19:46:40.5 UTC
    return {.size = size, .mtime_seconds = 1731700000, .mtime_nanoseconds = 500000000};
}

d::FilePlan plan(std::uint64_t size, Headers request, FileOptions options = {},
                 std::optional<ByteRange> range = std::nullopt,
                 std::string_view type = "audio/wav") {
    return d::plan_file_response(facts(size), type, range, options, request, "b0undary");
}

std::string body_of(const d::FilePlan& plan, const std::string& file) {
    std::string out = plan.body;
    for (const auto& piece : plan.pieces) {
        out += piece.literal;
        out += file.substr(piece.offset, piece.length);
    }
    return out;
}

std::string pattern(std::size_t size) {
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('a' + i % 26);
    }
    return data;
}

} // namespace

TEST_CASE("Range headers parse as Starlette's _parse_range_header") {
    const auto ranges = [](std::string_view header, std::uint64_t size) {
        return d::parse_range_header(header, size);
    };
    CHECK(ranges("bytes=0-99", 1000).ranges == Pairs{{0, 100}});
    CHECK(ranges("bytes=-10", 1000).ranges == Pairs{{990, 1000}});
    CHECK(ranges("bytes=-5000", 1000).ranges == Pairs{{0, 1000}});
    CHECK(ranges("bytes=10-", 1000).ranges == Pairs{{10, 1000}});
    CHECK(ranges("bytes=0-5000", 1000).ranges == Pairs{{0, 1000}});
    CHECK(ranges("Bytes = 1-2", 1000).ranges == Pairs{{1, 3}});
    CHECK(ranges("bytes=+1-2", 1000).ranges == Pairs{{1, 3}});
    CHECK(ranges("bytes=0-9, 5-19,50-59", 1000).ranges == Pairs{{0, 20}, {50, 60}});
    CHECK(ranges("bytes=50-59,0-9", 1000).ranges == Pairs{{0, 10}, {50, 60}});
    CHECK(ranges("bytes=0-9,x,-,", 1000).ranges == Pairs{{0, 10}});

    CHECK(ranges("bytes=500-", 100).kind == Kind::Unsatisfiable);
    CHECK(ranges("bytes=-0", 100).kind == Kind::Unsatisfiable);
    CHECK(ranges("bytes=0-0", 0).kind == Kind::Unsatisfiable);
    CHECK(ranges("bytes=--5", 100).kind == Kind::Unsatisfiable);
    CHECK(ranges("bytes=99999999999999999999999-", 100).kind == Kind::Unsatisfiable);

    const auto inverted = ranges("bytes=50-10", 100);
    CHECK(inverted.kind == Kind::Malformed);
    CHECK(inverted.message == "Range header: start must be less than end");
    CHECK(ranges("bytes=abc", 100).message == "Range header: range must be requested");
    CHECK(ranges("bytes=", 100).message == "Range header: range must be requested");
    CHECK(ranges("items=0-1", 100).message == "Only support bytes range");
    CHECK(ranges("0-1", 100).message == "Malformed range header.");

    std::string many = "bytes=0-1";
    for (int i = 0; i < 100; ++i) {
        many += ",0-1";
    }
    CHECK(ranges(many, 100).kind == Kind::Whole);
}

TEST_CASE("parse_range gives the one range a header names") {
    CHECK(parse_range("bytes=0-99", 1000)->first == 0u);
    CHECK(parse_range("bytes=0-99", 1000)->last == 99u);
    CHECK_FALSE(parse_range("bytes=-10", 1000)->first.has_value());
    CHECK(parse_range("bytes=-10", 1000)->last == 10u);
    CHECK(parse_range("bytes=5-", 1000)->first == 5u);
    CHECK_FALSE(parse_range("bytes=5-", 1000)->last.has_value());
    CHECK(parse_range("bytes=-0", 1000)->last == 0u);
    CHECK_FALSE(parse_range("bytes=0-1,2-3", 1000).has_value());
    CHECK_FALSE(parse_range("bytes=abc", 1000).has_value());
    CHECK_FALSE(parse_range("junk", 1000).has_value());
    CHECK_FALSE(parse_range("items=0-1", 1000).has_value());

    // A given range is checked as the header's single range is.
    CHECK(d::check_byte_range({.first = 0, .last = 99}, 1000).ranges == Pairs{{0, 100}});
    CHECK(d::check_byte_range({.first = std::nullopt, .last = 10}, 1000).ranges ==
          Pairs{{990, 1000}});
    CHECK(d::check_byte_range({.first = 500, .last = std::nullopt}, 100).kind ==
          Kind::Unsatisfiable);
    CHECK(d::check_byte_range({.first = 50, .last = 10}, 100).kind == Kind::Malformed);
    CHECK(d::check_byte_range({}, 100).kind == Kind::Whole);
}

TEST_CASE("parsedate reads dates as email.utils.parsedate does") {
    using Date = std::array<std::int64_t, 6>;
    CHECK(d::parsedate("Fri, 15 Nov 2024 19:46:40 GMT") == Date{2024, 11, 15, 19, 46, 40});
    CHECK(d::parsedate("Sunday, 06-Nov-94 08:49:37 GMT") == Date{1994, 11, 6, 8, 49, 37});
    CHECK(d::parsedate("Sun Nov  6 08:49:37 1994") == Date{1994, 11, 6, 8, 49, 37});
    CHECK(d::parsedate("6 Nov 2068 08:49 +0200") == Date{2068, 11, 6, 8, 49, 0});
    CHECK(d::parsedate("6 November 69 08.49.37 EST") == Date{1969, 11, 6, 8, 49, 37});
    CHECK_FALSE(d::parsedate("").has_value());
    CHECK_FALSE(d::parsedate("   ").has_value());
    CHECK_FALSE(d::parsedate("garbage").has_value());
    CHECK_FALSE(d::parsedate("6 Nox 2024 08:49:37").has_value());
    CHECK_FALSE(d::parsedate("6 Nov 2024 08").has_value());
}

TEST_CASE("content types come from Python's table") {
    CHECK(content_type_for("index.html") == "text/html");
    CHECK(content_type_for("assets/index-BqkSL9Ir.JS") == "text/javascript");
    CHECK(content_type_for("index--F1caWmT.css") == "text/css");
    CHECK(content_type_for("DMSans.woff2") == "font/woff2");
    CHECK(content_type_for("DMSans.woff") == "font/woff");
    CHECK(content_type_for("meeting.wav") == "audio/x-wav");
    CHECK(content_type_for("logo.svg") == "image/svg+xml");
    CHECK(content_type_for("logo.svgz") == "image/svg+xml");
    CHECK(content_type_for("backup.tar.gz") == "application/x-tar");
    CHECK(content_type_for("backup.tgz") == "application/x-tar");
    CHECK(content_type_for("notes.gz") == "application/octet-stream");
    CHECK(content_type_for(".bashrc") == "application/octet-stream");
    CHECK(content_type_for("README") == "application/octet-stream");
    CHECK(content_type_for("x.unknown") == "application/octet-stream");
}

TEST_CASE("a whole file answers FileResponse's headers in its order") {
    const auto answer = plan(123, {});
    CHECK(answer.status == 200);
    CHECK(answer.headers == Headers{{"content-type", "audio/wav"},
                                    {"accept-ranges", "bytes"},
                                    {"content-length", "123"},
                                    {"last-modified", "Fri, 15 Nov 2024 19:46:40 GMT"},
                                    {"etag", "\"2a74f08e49e90a8a1a44f2f2c51a20da\""}});
    CHECK(answer.content_length == 123);
    REQUIRE(answer.pieces.size() == 1);
    CHECK(answer.pieces[0].offset == 0);
    CHECK(answer.pieces[0].length == 123);
}

TEST_CASE("the caller's headers, text charset and the disposition") {
    FileOptions options;
    options.headers = {{"Cache-Control", "no-store"}};
    options.filename = "meeting-1.wav";
    options.content_disposition_type = "inline";
    const auto inline_wav = plan(10, {}, options);
    CHECK(inline_wav.headers.at(0) ==
          std::pair<std::string, std::string>{"cache-control", "no-store"});
    CHECK(header(inline_wav.headers, "content-disposition") ==
          "inline; filename=\"meeting-1.wav\"");

    options.filename = "r\xc3\xa9union 1.wav";
    options.content_disposition_type = "attachment";
    CHECK(header(plan(10, {}, options).headers, "content-disposition") ==
          "attachment; filename*=utf-8''r%C3%A9union%201.wav");

    CHECK(header(plan(10, {}, {}, std::nullopt, "text/html").headers, "content-type") ==
          "text/html; charset=utf-8");
    CHECK(header(plan(10, {}, {}, std::nullopt, "text/html; charset=latin-1").headers,
                 "content-type") == "text/html; charset=latin-1");
}

TEST_CASE("one range answers 206 with Content-Range") {
    const auto answer = plan(1000, {{"Range", "bytes=0-99"}});
    CHECK(answer.status == 206);
    CHECK(header(answer.headers, "content-range") == "bytes 0-99/1000");
    CHECK(header(answer.headers, "content-length") == "100");
    CHECK(answer.headers.back().first == "content-range");
    REQUIRE(answer.pieces.size() == 1);
    CHECK(answer.pieces[0].offset == 0);
    CHECK(answer.pieces[0].length == 100);

    const auto last_ten = plan(1000, {{"range", "bytes=-10"}});
    CHECK(header(last_ten.headers, "content-range") == "bytes 990-999/1000");
}

TEST_CASE("several ranges answer multipart/byteranges") {
    const std::string file = pattern(100);
    const auto answer = plan(100, {{"Range", "bytes=0-4,10-14"}});
    CHECK(answer.status == 206);
    CHECK(header(answer.headers, "content-type") == "multipart/byteranges; boundary=b0undary");
    const std::string expected = "--b0undary\r\nContent-Type: audio/wav\r\n"
                                 "Content-Range: bytes 0-4/100\r\n\r\nabcde\r\n"
                                 "--b0undary\r\nContent-Type: audio/wav\r\n"
                                 "Content-Range: bytes 10-14/100\r\n\r\nklmno\r\n"
                                 "--b0undary--";
    CHECK(body_of(answer, file) == expected);
    CHECK(header(answer.headers, "content-length") == std::to_string(expected.size()));
    CHECK(answer.content_length == expected.size());
}

TEST_CASE("unsatisfiable and malformed ranges answer as Starlette does") {
    const auto past = plan(300, {{"Range", "bytes=500-"}});
    CHECK(past.status == 416);
    CHECK(past.headers == Headers{{"content-range", "bytes */300"},
                                  {"content-length", "0"},
                                  {"content-type", "text/plain; charset=utf-8"}});
    CHECK(past.body.empty());
    CHECK(past.pieces.empty());

    const auto inverted = plan(300, {{"Range", "bytes=50-10"}});
    CHECK(inverted.status == 400);
    CHECK(inverted.body == "Range header: start must be less than end");
    CHECK(header(inverted.headers, "content-type") == "text/plain; charset=utf-8");

    CHECK(plan(300, {{"Range", "bytes=-0"}}).status == 416);
}

TEST_CASE("If-Range must name the file's date or ETag") {
    const std::string etag = d::file_etag(facts(1000));
    CHECK(plan(1000, {{"Range", "bytes=0-9"}, {"If-Range", etag}}).status == 206);
    CHECK(plan(1000, {{"Range", "bytes=0-9"}, {"If-Range", "Fri, 15 Nov 2024 19:46:40 GMT"}})
              .status == 206);
    CHECK(plan(1000, {{"Range", "bytes=0-9"}, {"If-Range", "\"other\""}}).status == 200);
}

TEST_CASE("StaticFiles answers 304 for a matching ETag or a date not older") {
    FileOptions statics;
    statics.not_modified_check = true;
    const std::string etag = d::file_etag(facts(10));

    const auto matching = plan(10, {{"If-None-Match", etag}}, statics);
    CHECK(matching.status == 304);
    CHECK(matching.headers == Headers{{"etag", etag}});
    CHECK(matching.pieces.empty());
    CHECK(plan(10, {{"If-None-Match", "\"x\", W/" + etag}}, statics).status == 304);
    CHECK(plan(10, {{"If-None-Match", "\"x\""}}, statics).status == 200);
    // If-None-Match decides alone when present.
    CHECK(plan(10, {{"If-None-Match", "\"x\""}, {"If-Modified-Since", "Sat, 1 Jan 2099 0:0:0"}},
               statics)
              .status == 200);

    CHECK(plan(10, {{"If-Modified-Since", "Fri, 15 Nov 2024 19:46:40 GMT"}}, statics).status ==
          304);
    CHECK(plan(10, {{"If-Modified-Since", "Fri, 15 Nov 2024 19:46:39 GMT"}}, statics).status ==
          200);
    CHECK(plan(10, {{"If-Modified-Since", "garbage"}}, statics).status == 200);

    FileOptions cached = statics;
    cached.headers = {{"cache-control", "no-cache"}};
    CHECK(plan(10, {{"If-None-Match", etag}}, cached).headers ==
          Headers{{"cache-control", "no-cache"}, {"etag", etag}});

    // FileResponse alone never answers 304.
    CHECK(plan(10, {{"If-None-Match", etag}}).status == 200);
}

TEST_CASE("the ETag hashes repr(st_mtime) and the size") {
    // md5(b"1731700000.5-123"), as Python computes it
    CHECK(d::file_etag(facts(123)) == "\"2a74f08e49e90a8a1a44f2f2c51a20da\"");
    CHECK(d::file_last_modified(facts(1)) == "Fri, 15 Nov 2024 19:46:40 GMT");
}

namespace {

// A server whose routes send files from a temporary folder.
struct FileFixture {
    testkit::TempDataRoot root;
    Reactor reactor;
    std::unique_ptr<HttpServer> server = HttpServer::create(reactor);
    Endpoint endpoint;
    std::filesystem::path file = root.path() / "audio.wav";
    std::string content = pattern(300);

    FileFixture() {
        std::ofstream(file, std::ios::binary) << content;
        server->route("GET", "/file", [this](const HttpRequest&, std::shared_ptr<Responder> r) {
            r->send_file(file, "audio/wav", std::nullopt);
        });
        server->route("GET", "/static", [this](const HttpRequest&, std::shared_ptr<Responder> r) {
            FileOptions options;
            options.not_modified_check = true;
            r->send_file(file, "", std::nullopt, options);
        });
        server->route("GET", "/given", [this](const HttpRequest&, std::shared_ptr<Responder> r) {
            r->send_file(file, "audio/wav", ByteRange{.first = 10, .last = 19});
        });
        server->route("GET", "/named", [this](const HttpRequest&, std::shared_ptr<Responder> r) {
            // A file whose own name guesses nothing, sent under a download name that does.
            const auto blob = root.path() / "spool.part";
            std::filesystem::copy_file(file, blob,
                                       std::filesystem::copy_options::overwrite_existing);
            FileOptions options;
            options.filename = "meeting-1.json";
            options.content_disposition_type = "inline";
            r->send_file(blob, "", std::nullopt, options);
        });
        server->route("GET", "/missing", [this](const HttpRequest&, std::shared_ptr<Responder> r) {
            r->send_file(root.path() / "nothing.wav", "audio/wav", std::nullopt);
        });
        endpoint = t::listen_loopback(*server);
    }
    ~FileFixture() { server->stop(0ms); }

    std::string get(std::string_view path, std::string_view extra = {}, bool head = false) {
        t::RawClient client(endpoint.port);
        client.send(std::string(head ? "HEAD " : "GET ") + std::string(path) +
                    " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(extra) + "\r\n");
        return client.read_response(head);
    }
};

std::string body_after_head(const std::string& response) {
    const auto end = response.find("\r\n\r\n");
    return end == std::string::npos ? std::string() : response.substr(end + 4);
}

} // namespace

TEST_CASE("send_file answers the whole file with its headers", "[platform]") {
    FileFixture f;
    const auto response = f.get("/file");
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    const auto lowered = t::RawClient::lower(response);
    CHECK(lowered.contains("\r\ncontent-type: audio/wav\r\n"));
    CHECK(lowered.contains("\r\naccept-ranges: bytes\r\n"));
    CHECK(lowered.contains("\r\ncontent-length: 300\r\n"));
    CHECK(lowered.contains("\r\nlast-modified: "));
    CHECK(lowered.contains("\r\netag: \""));
    CHECK(body_after_head(response) == f.content);
}

TEST_CASE("send_file answers ranges with 206 and 416", "[platform]") {
    FileFixture f;
    const auto first = f.get("/file", "Range: bytes=0-99\r\n");
    CHECK(first.starts_with("HTTP/1.1 206 Partial Content\r\n"));
    CHECK(first.contains("\r\ncontent-range: bytes 0-99/300\r\n"));
    CHECK(body_after_head(first) == f.content.substr(0, 100));

    const auto last = f.get("/file", "Range: bytes=-10\r\n");
    CHECK(last.contains("\r\ncontent-range: bytes 290-299/300\r\n"));
    CHECK(body_after_head(last) == f.content.substr(290));

    const auto past = f.get("/file", "Range: bytes=500-\r\n");
    // Python 3.12's HTTPStatus phrase, which uvicorn's status line uses.
    CHECK(past.starts_with("HTTP/1.1 416 Requested Range Not Satisfiable\r\n"));
    CHECK(past.contains("\r\ncontent-range: bytes */300\r\n"));
    CHECK(body_after_head(past).empty());

    const auto given = f.get("/given");
    CHECK(given.contains("\r\ncontent-range: bytes 10-19/300\r\n"));
    CHECK(body_after_head(given) == f.content.substr(10, 10));
}

TEST_CASE("send_file answers 304 for a matching ETag with no body", "[platform]") {
    FileFixture f;
    const auto full = f.get("/static");
    const auto at = t::RawClient::lower(full).find("\r\netag: ");
    REQUIRE(at != std::string::npos);
    const auto etag = full.substr(at + 8, full.find("\r\n", at + 8) - (at + 8));
    CHECK(full.contains("\r\ncontent-type: audio/x-wav\r\n")); // guessed from the name

    t::RawClient client(f.endpoint.port);
    client.send("GET /static HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + etag + "\r\n\r\n");
    const auto head = client.read_until("\r\n\r\n");
    CHECK(head.starts_with("HTTP/1.1 304 Not Modified\r\n"));
    CHECK_FALSE(t::RawClient::lower(head).contains("content-length"));
    // The connection stays open and the next answer follows directly: no body was sent.
    client.send("GET /file HTTP/1.1\r\nHost: x\r\nRange: bytes=0-2\r\n\r\n");
    const auto next = client.read_response();
    CHECK(next.starts_with("HTTP/1.1 206 Partial Content\r\n"));
    CHECK(body_after_head(next) == "abc");
}

TEST_CASE("send_file guesses the type from the download name first, as FileResponse does",
          "[platform]") {
    // FileResponse: guess_type(filename or path)[0].
    FileFixture f;
    const auto response = f.get("/named");
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    const auto lowered = t::RawClient::lower(response);
    CHECK(lowered.contains("\r\ncontent-type: application/json\r\n"));
    CHECK(lowered.contains("\r\ncontent-disposition: inline; filename=\"meeting-1.json\"\r\n"));
    CHECK(body_after_head(response) == f.content);
}

TEST_CASE("send_file of a missing file is FastAPI's 404", "[platform]") {
    FileFixture f;
    const auto response = f.get("/missing");
    CHECK(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
    CHECK(body_after_head(response) == R"({"detail":"Not Found"})");
}

TEST_CASE("HEAD of a file sends the head alone", "[platform]") {
    FileFixture f;
    t::RawClient client(f.endpoint.port);
    client.send("HEAD /file HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto head = client.read_response(true);
    CHECK(head.contains("\r\ncontent-length: 300\r\n"));
    client.send("GET /file HTTP/1.1\r\nHost: x\r\nRange: bytes=1-1\r\n\r\n");
    CHECK(body_after_head(client.read_response()) == "b");
}

TEST_CASE("several ranges come back as one multipart body", "[platform]") {
    FileFixture f;
    const auto response = f.get("/file", "Range: bytes=0-1, 4-5\r\n");
    CHECK(response.starts_with("HTTP/1.1 206 Partial Content\r\n"));
    const auto at = response.find("boundary=");
    REQUIRE(at != std::string::npos);
    const auto boundary = response.substr(at + 9, 26);
    const std::string expected = "--" + boundary + "\r\nContent-Type: audio/wav\r\n" +
                                 "Content-Range: bytes 0-1/300\r\n\r\nab\r\n--" + boundary +
                                 "\r\nContent-Type: audio/wav\r\nContent-Range: bytes 4-5/300" +
                                 "\r\n\r\nef\r\n--" + boundary + "--";
    CHECK(body_after_head(response) == expected);
}

namespace {

#ifdef _WIN32
std::size_t private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof(counters));
    return counters.PrivateUsage;
}
#else
std::size_t private_bytes() {
    return 0;
}
#endif

} // namespace

TEST_CASE("a 300 MB file is streamed without being read whole", "[platform]") {
    testkit::TempDataRoot root;
    const auto big = root.path() / "big.wav";
    constexpr std::uint64_t kSize = 300ull << 20;
    std::ofstream(big, std::ios::binary).put('R');
    // Extending the file writes no data: the rest reads as zeros.
    std::filesystem::resize_file(big, kSize);

    Reactor reactor;
    auto server = HttpServer::create(reactor);
    server->route("GET", "/big", [&](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send_file(big, "audio/wav", std::nullopt);
    });
    const auto endpoint = t::listen_loopback(*server);
    const std::size_t before = private_bytes();

    t::RawClient client(endpoint.port);
    client.send("GET /big HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto head = client.read_until("\r\n\r\n");
    CHECK(head.contains("\r\ncontent-length: 314572800\r\n"));
    // The body's first bytes may have come with the head.
    const std::string early = client.take_pending();
    std::uint64_t received = early.size();
    std::size_t peak = before;
    std::vector<char> chunk(256 * 1024);
    boost::system::error_code error;
    char first = early.empty() ? 0 : early.front();
    while (received < kSize) {
        const auto n = client.socket().read_some(boost::asio::buffer(chunk), error);
        if (error) {
            break;
        }
        if (received == 0 && n > 0) {
            first = chunk[0];
        }
        received += n;
        if ((received >> 20) % 16 == 0) {
            peak = std::max(peak, private_bytes());
        }
    }
    CHECK(received == kSize);
    CHECK(first == 'R');
    // A file read whole would add 300 MB; the stream holds a few 64 KiB chunks.
    CHECK(peak - before < (64u << 20));
    server->stop(0ms);
}

TEST_CASE("stop ends a file response the client stopped reading after the grace", "[platform]") {
    testkit::TempDataRoot root;
    const auto big = root.path() / "big.wav";
    std::ofstream(big, std::ios::binary).put('R');
    std::filesystem::resize_file(big, 64ull << 20);

    Reactor reactor;
    auto server = HttpServer::create(reactor);
    server->route("GET", "/big", [&](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send_file(big, "audio/wav", std::nullopt);
    });
    const auto endpoint = t::listen_loopback(*server);
    t::RawClient client(endpoint.port);
    client.send("GET /big HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_FALSE(client.read_until("\r\n\r\n").empty());

    // The dashboard's player keeps such a response open; stop must not wait on it (< 5 s).
    const auto started = std::chrono::steady_clock::now();
    server->stop(1s);
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(took < 5s);
    CHECK(took >= 900ms);
}
