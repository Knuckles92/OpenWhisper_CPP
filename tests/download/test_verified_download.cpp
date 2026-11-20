// Verified, resumable downloads: OpenWhisper's component download tests, the resume decision
// table and error texts replayed from Python (tests/golden/services.verified_download.json), and
// the whole path through the curl client against servers on the loopback interface.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../http/loopback_server.h"
#include "base/hash.h"
#include "download/verified_download.h"
#include "golden/golden.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "http/user_agent.h"
#include "testkit/testkit.h"

using fv::download::DownloadCanceled;
using fv::download::DownloadError;
using fv::download::DownloadInterrupted;
using fv::download::NetworkFailure;
using fv::http::FakeHttpClient;
using fv::http::ScriptedResponse;
using fv::testing::LoopbackServer;
namespace fs = std::filesystem;

namespace {

const std::string kBody = "component-payload";
const std::string kUrl = "https://files.pythonhosted.org/packages/example.whl";

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string host_of(std::string_view url) {
    auto rest = url.substr(url.find("://") + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    return std::string(rest.substr(0, rest.rfind(':') == std::string_view::npos ||
                                              rest.find(']') != std::string_view::npos
                                          ? rest.size()
                                          : rest.rfind(':')));
}

// components._download_verified: the URL's own host named for certificate failures, and a
// cancelled transfer kept for resume.
fs::path fetch(fv::http::Client& client, const fs::path& folder, const std::string& url = kUrl,
               const fv::CancelToken& cancel = {}) {
    const auto destination = folder / "example.whl";
    fv::download::DownloadOptions options;
    const auto host = host_of(url);
    options.describe_error = [host](const NetworkFailure& failure) {
        fv::download::NetworkErrorText text;
        text.hosts = {host};
        return fv::download::describe_network_error(failure, text);
    };
    fv::download::download_verified(
        client, url, destination, static_cast<std::int64_t>(kBody.size()), fv::sha256_hex(kBody),
        [](std::string_view, std::int64_t, std::int64_t) {}, cancel, true, options);
    return destination;
}

ScriptedResponse respond(int status, std::string body, fv::http::Headers headers = {}) {
    ScriptedResponse response;
    response.status = status;
    response.reason = status == 206 ? "Partial Content" : "OK";
    response.body = std::move(body);
    response.headers = std::move(headers);
    return response;
}

std::string message_of(const std::function<void()>& action) {
    std::optional<std::string> message;
    try {
        action();
    } catch (const DownloadError& error) {
        message = error.what();
    }
    REQUIRE(message.has_value());
    return *message;
}

} // namespace

// from tests/test_component_download.py::test_a_resume_with_the_wrong_range_is_discarded
TEST_CASE("test_a_resume_with_the_wrong_range_is_discarded", "[test_component_download]") {
    const fv::testkit::TempDataRoot root;
    write_file(root.path() / "example.whl.part", kBody.substr(0, 8));
    FakeHttpClient client;
    client.push(respond(206, kBody.substr(8), {{"Content-Range", "bytes 0-9/17"}}));
    const auto message = message_of([&] { fetch(client, root.path()); });
    CHECK(message.find("invalid resume response") != std::string::npos);
    CHECK_FALSE(fs::exists(root.path() / "example.whl.part"));
}

// from tests/test_component_download.py::test_a_corrupt_download_is_discarded
TEST_CASE("test_a_corrupt_download_is_discarded", "[test_component_download]") {
    const fv::testkit::TempDataRoot root;
    const auto folder = root.path() / "downloads";
    fs::create_directories(folder);
    FakeHttpClient client;
    client.push(respond(200, std::string(kBody.size(), 'x')));
    const auto message = message_of([&] { fetch(client, folder); });
    CHECK(message.find("integrity check") != std::string::npos);
    CHECK(fs::is_empty(folder));
}

// from tests/test_component_download.py::test_a_network_failure_keeps_the_partial
TEST_CASE("test_a_network_failure_keeps_the_partial", "[test_component_download]") {
    const fv::testkit::TempDataRoot root;
    write_file(root.path() / "example.whl.part", kBody.substr(0, 8));
    FakeHttpClient client;
    ScriptedResponse offline;
    offline.error = fv::http::Error{fv::http::ErrorKind::Connect, "no route to host"};
    client.push(offline);
    const auto message = message_of([&] { fetch(client, root.path()); });
    CHECK(message.find("Could not reach the download server") != std::string::npos);
    CHECK(read_file(root.path() / "example.whl.part") == kBody.substr(0, 8));
}

// from tests/test_component_download.py::test_certificate_failure_names_the_host_being_contacted
TEST_CASE("test_certificate_failure_names_the_host_being_contacted", "[test_component_download]") {
    // Payloads come from several hosts, so the message must name this one.
    const fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    ScriptedResponse intercepted;
    intercepted.error = FakeHttpClient::certificate_failure();
    client.push(intercepted);
    const auto message = message_of([&] { fetch(client, root.path()); });
    CHECK(message.find("certificate could not be verified") != std::string::npos);
    CHECK(message.find("allow files.pythonhosted.org.") != std::string::npos);
}

// from tests/test_component_download.py::test_certificate_message_for_a_model_host
TEST_CASE("test_certificate_message_for_a_model_host", "[test_component_download]") {
    const fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    ScriptedResponse intercepted;
    intercepted.error = FakeHttpClient::certificate_failure();
    client.push(intercepted);
    const auto message = message_of([&] {
        fetch(client, root.path(), "https://huggingface.co/nvidia/model/resolve/main/model.gguf");
    });
    CHECK(message.find("allow huggingface.co.") != std::string::npos);
    CHECK(message.find("pythonhosted") == std::string::npos);
}

namespace {

const std::string kGoldenBody = "component-payload";

std::string golden_bytes(const fv::Json& spec) {
    if (spec.is_object()) {
        return std::string(spec.at("junk").get<std::size_t>(), 'x');
    }
    if (!spec.empty() && (spec[0].is_array() || spec[0].is_object())) {
        std::string joined;
        for (const auto& part : spec) {
            joined += golden_bytes(part);
        }
        return joined;
    }
    const auto from = std::min<std::size_t>(spec[0].get<std::size_t>(), kGoldenBody.size());
    const auto to = std::min<std::size_t>(spec[1].get<std::size_t>(), kGoldenBody.size());
    return to > from ? kGoldenBody.substr(from, to - from) : std::string();
}

fv::Json optional_file(const fs::path& path) {
    if (!fs::exists(path)) {
        return nullptr;
    }
    return read_file(path);
}

} // namespace

TEST_CASE("the resume decisions match Python's", "[golden][verified_download]") {
    fv::golden::for_each("services.verified_download", "resume", [](const fv::golden::Case& c) {
        const fv::testkit::TempDataRoot root;
        const auto destination = root.path() / "payload.bin";
        const auto part = root.path() / "payload.bin.part";
        if (!c.args()[0].is_null()) {
            write_file(part, golden_bytes(c.args()[0]));
        }
        const auto& options = c.args()[2];

        fv::CancelSource cancel;
        FakeHttpClient client;
        for (const auto& spec : c.args()[1]) {
            ScriptedResponse response;
            if (spec.contains("url_error")) {
                response.error = fv::http::Error{fv::http::ErrorKind::Connect,
                                                 spec.at("url_error").get<std::string>()};
            } else if (spec.contains("certificate")) {
                response.error = FakeHttpClient::certificate_failure();
            } else {
                response.status = spec.value("status", 200);
                response.reason = spec.value("reason", "");
                for (const auto& header : spec.value("headers", fv::Json::array())) {
                    response.headers.emplace_back(header[0].get<std::string>(),
                                                  header[1].get<std::string>());
                }
                response.body = golden_bytes(spec.value("body", fv::Json::array({0, 0})));
                if (spec.contains("fail")) {
                    const auto text = spec.at("fail").get<std::string>();
                    response.cut =
                        fv::http::Error{text == "timed out" ? fv::http::ErrorKind::Timeout
                                                            : fv::http::ErrorKind::Connect,
                                        text};
                }
                if (options.value("cancel_on_read", false)) {
                    response.on_chunk = [&cancel] {
                        cancel.cancel();
                    };
                }
            }
            client.push(std::move(response));
        }

        fv::Json progress = fv::Json::array();
        fv::download::DownloadOptions download_options;
        if (options.contains("max_bytes")) {
            download_options.max_bytes = options.at("max_bytes").get<std::int64_t>();
        }
        download_options.describe_error = [](const NetworkFailure& failure) {
            fv::download::NetworkErrorText text;
            text.hosts = {"files.example.test"};
            return fv::download::describe_network_error(failure, text);
        };
        fv::Json outcome = "ok";
        fv::Json message = nullptr;
        try {
            fv::download::download_verified(
                client, "https://files.example.test/payload.bin", destination,
                options.value("size", static_cast<std::int64_t>(kGoldenBody.size())),
                fv::sha256_hex(kGoldenBody),
                [&progress](std::string_view phase, std::int64_t done, std::int64_t total) {
                    progress.push_back(fv::Json::array({std::string(phase), done, total}));
                },
                cancel.token(), options.value("keep_partial_on_cancel", false), download_options);
        } catch (const DownloadCanceled& error) {
            outcome = "DownloadCanceled";
            message = error.what();
        } catch (const DownloadInterrupted& error) {
            outcome = "DownloadInterrupted";
            message = error.what();
        } catch (const DownloadError& error) {
            outcome = "DownloadError";
            message = error.what();
        }

        fv::Json requests = fv::Json::array();
        for (const auto& request : client.requests()) {
            requests.push_back(request.range_from
                                   ? fv::Json("bytes=" + std::to_string(*request.range_from) + "-")
                                   : fv::Json(nullptr));
        }
        const fv::Json actual = {
            {"outcome", outcome},
            {"message", message},
            {"requests", requests},
            {"part", optional_file(part)},
            {"destination", optional_file(destination)},
            {"progress", progress},
        };
        const auto expected = c.result<fv::Json>();
        for (const auto& key :
             {"outcome", "message", "requests", "part", "destination", "progress"}) {
            CAPTURE(key);
            CHECK(fv::golden::equal(expected.at(key), actual.at(key)));
            if (!fv::golden::equal(expected.at(key), actual.at(key))) {
                UNSCOPED_INFO("expected " << expected.at(key).dump() << " got "
                                          << actual.at(key).dump());
            }
        }
    });
}

TEST_CASE("network error texts match Python's", "[golden][verified_download]") {
    fv::golden::for_each("services.verified_download", "describe", [](const fv::golden::Case& c) {
        const auto kind = c.arg<std::string>(0);
        const auto& detail = c.args()[1];
        NetworkFailure failure;
        if (kind == "certificate" || kind == "certificate_direct") {
            failure.kind = NetworkFailure::Kind::Certificate;
        } else if (kind == "http") {
            failure.kind = NetworkFailure::Kind::HttpStatus;
            failure.code = detail[0].get<int>();
            failure.reason = detail[1].get<std::string>();
        } else if (kind == "url") {
            failure.kind = NetworkFailure::Kind::Url;
            failure.reason = detail.get<std::string>();
        } else {
            failure.kind = NetworkFailure::Kind::Other;
            failure.reason = detail.get<std::string>();
        }
        fv::download::NetworkErrorText text;
        if (const auto server = c.arg<std::optional<std::string>>(2)) {
            text.server = *server;
        }
        text.hosts = c.arg<std::vector<std::string>>(3);
        for (const auto& pair : c.args()[4]) {
            text.http_messages[pair[0].get<int>()] = pair[1].get<std::string>();
        }
        CHECK(fv::download::describe_network_error(failure, text) == c.result<std::string>());
    });
}

TEST_CASE("an http::Error stands for the failure Python would have raised", "[verified_download]") {
    fv::http::Error refused{fv::http::ErrorKind::Connect, "[WinError 10061] refused"};
    CHECK(fv::download::network_failure(refused).kind == NetworkFailure::Kind::Url);
    refused.after_head = true;
    CHECK(fv::download::network_failure(refused).kind == NetworkFailure::Kind::Other);
    CHECK(fv::download::network_failure(refused).reason == "[WinError 10061] refused");
    fv::http::Error unanswered{fv::http::ErrorKind::Timeout, "timed out"};
    unanswered.after_request = true;
    CHECK(fv::download::network_failure(unanswered).kind == NetworkFailure::Kind::Other);
    CHECK(fv::download::network_failure(FakeHttpClient::certificate_failure()).kind ==
          NetworkFailure::Kind::Certificate);
}

TEST_CASE("the user's messages replace the built-in ones", "[verified_download]") {
    const fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.push(respond(200, "short"));
    fv::download::DownloadOptions options;
    options.messages.incomplete = "Only {done} of {total} arrived; {done} kept.";
    try {
        fv::download::download_verified(
            client, "https://x.test/f", root.path() / "f", 17, fv::sha256_hex(kBody),
            [](std::string_view, std::int64_t, std::int64_t) {}, {}, false, options);
        FAIL("no error");
    } catch (const DownloadInterrupted& error) {
        CHECK(std::string(error.what()) == "Only 5 B of 17 B arrived; 5 B kept.");
    }
    const auto request = client.requests().at(0);
    CHECK(fv::http::find_header(request.headers, "User-Agent") == fv::http::user_agent());
    CHECK(request.follow_redirects);
    CHECK(request.connect_timeout == std::chrono::seconds(30));
    CHECK(request.idle_timeout == std::chrono::seconds(30));
}

TEST_CASE("a partial that is a hard link is not trusted", "[verified_download][platform]") {
    const fv::testkit::TempDataRoot root;
    const auto other = root.path() / "elsewhere.bin";
    write_file(other, kBody.substr(0, 8));
    fs::create_hard_link(other, root.path() / "example.whl.part");
    FakeHttpClient client;
    const auto message = message_of([&] { fetch(client, root.path()); });
    CHECK(message == "The partial download is not a regular file.");
    CHECK(client.requests().empty());
    CHECK_FALSE(fs::exists(root.path() / "example.whl.part"));
    CHECK(read_file(other) == kBody.substr(0, 8));
}

TEST_CASE("a partial that is a directory cannot be created safely",
          "[verified_download][platform]") {
    const fv::testkit::TempDataRoot root;
    fs::create_directories(root.path() / "example.whl.part");
    FakeHttpClient client;
    CHECK(message_of([&] { fetch(client, root.path()); }) ==
          "The download could not be created safely.");
    CHECK(fs::is_directory(root.path() / "example.whl.part"));
}

TEST_CASE("a finished download replaces the file already there", "[verified_download]") {
    const fv::testkit::TempDataRoot root;
    write_file(root.path() / "example.whl", "old contents");
    FakeHttpClient client;
    client.push(respond(200, kBody, {{"Content-Length", "17"}}));
    const auto destination = fetch(client, root.path());
    CHECK(read_file(destination) == kBody);
    CHECK_FALSE(fs::exists(root.path() / "example.whl.part"));
}

namespace {

// A static file server for kBody that honours Range; `cut_first` ends the first response after
// eight bytes, as a dropped connection does.
LoopbackServer::Handler file_server(bool honour_range, bool cut_first = false) {
    auto first = std::make_shared<bool>(true);
    return [=](const LoopbackServer::Request& request, LoopbackServer::Reply& reply) {
        const auto range = request.header("Range");
        if (*first && cut_first) {
            *first = false;
            reply.write("HTTP/1.1 200 OK\r\nContent-Length: 17\r\nConnection: close\r\n\r\n" +
                        kBody.substr(0, 8));
            return;
        }
        *first = false;
        if (range && honour_range) {
            const auto from = std::stoul(range->substr(6));
            reply.write(LoopbackServer::response(
                206, "Partial Content",
                {{"Content-Range", "bytes " + std::to_string(from) + "-16/17"}},
                kBody.substr(from)));
            return;
        }
        reply.write(LoopbackServer::response(200, "OK", {}, kBody));
    };
}

std::shared_ptr<fv::http::Client> direct_client() {
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return fv::http::make_curl_client(options);
}

} // namespace

TEST_CASE("a dropped connection is resumed with a checked Range", "[verified_download]") {
    LoopbackServer server(file_server(true, true));
    const fv::testkit::TempDataRoot root;
    const auto client = direct_client();
    CHECK_THROWS_AS(fetch(*client, root.path(), server.url("/example.whl")), DownloadInterrupted);
    CHECK(read_file(root.path() / "example.whl.part") == kBody.substr(0, 8));

    const auto destination = fetch(*client, root.path(), server.url("/example.whl"));
    CHECK(read_file(destination) == kBody);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK_FALSE(requests[0].header("Range"));
    CHECK(requests[1].header("Range") == "bytes=8-");
    CHECK(requests[1].header("User-Agent") == fv::http::user_agent());
}

TEST_CASE("a server that ignores Range sends the whole file again", "[verified_download]") {
    LoopbackServer server(file_server(false));
    const fv::testkit::TempDataRoot root;
    write_file(root.path() / "example.whl.part", kBody.substr(0, 8));
    const auto destination = fetch(*direct_client(), root.path(), server.url("/example.whl"));
    CHECK(read_file(destination) == kBody);
    CHECK(server.requests().at(0).header("Range") == "bytes=8-");
}

TEST_CASE("a download follows the server's redirect", "[verified_download]") {
    LoopbackServer files(file_server(true));
    LoopbackServer server([&files](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(302, "Found",
                                             {{"Location", files.url("/cdn/example.whl")}}, ""));
    });
    const fv::testkit::TempDataRoot root;
    const auto destination = fetch(*direct_client(), root.path(), server.url("/example.whl"));
    CHECK(read_file(destination) == kBody);
    CHECK(files.requests().at(0).target == "/cdn/example.whl");
}

TEST_CASE("an HTTP error leaves the partial for the next attempt", "[verified_download]") {
    LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(503, "Service Unavailable", {}, "busy"));
    });
    const fv::testkit::TempDataRoot root;
    write_file(root.path() / "example.whl.part", kBody.substr(0, 8));
    const auto message =
        message_of([&] { fetch(*direct_client(), root.path(), server.url("/example.whl")); });
    CHECK(message == "The download server returned an error (503 Service Unavailable).");
    CHECK(read_file(root.path() / "example.whl.part") == kBody.substr(0, 8));
}

TEST_CASE("a certificate failure through curl names the URL's host", "[verified_download]") {
    LoopbackServer server(file_server(true), true);
    const fv::testkit::TempDataRoot root;
    const auto message =
        message_of([&] { fetch(*direct_client(), root.path(), server.url("/example.whl")); });
    CHECK(message == "The download server's certificate could not be verified. This is usually "
                     "caused by network security software that inspects HTTPS traffic. Ask your "
                     "IT team to allow 127.0.0.1.");
}

TEST_CASE("a server that takes the request and never answers is worded as Python words it",
          "[verified_download]") {
    // urllib raises the bare TimeoutError or RemoteDisconnected from getresponse(), not a
    // URLError, so describe_network_error gives str() of it (checked against CPython 3.12.10).
    const auto message_from = [](const std::string& url) {
        const fv::testkit::TempDataRoot root;
        fv::download::DownloadOptions options;
        options.prepare = [](fv::http::Request& request) {
            request.idle_timeout = std::chrono::seconds(1);
        };
        const auto message = message_of([&] {
            fv::download::download_verified(
                *direct_client(), url, root.path() / "example.whl", 17, fv::sha256_hex(kBody),
                [](std::string_view, std::int64_t, std::int64_t) {}, {}, true, options);
        });
        // A network failure keeps the (here empty) partial for the next attempt.
        CHECK(fs::exists(root.path() / "example.whl.part"));
        return message;
    };

    LoopbackServer silent([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.wait(std::chrono::seconds(30));
    });
    CHECK(message_from(silent.url("/example.whl")) == "timed out");

    LoopbackServer hangup([](const LoopbackServer::Request&, LoopbackServer::Reply&) {});
    CHECK(message_from(hangup.url("/example.whl")) ==
          "Remote end closed connection without response");

    std::string gone;
    {
        LoopbackServer closed([](const LoopbackServer::Request&, LoopbackServer::Reply&) {});
        gone = closed.url("/example.whl");
    }
    LoopbackServer redirect([&gone](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(302, "Found", {{"Location", gone}}, ""));
    });
    CHECK(message_from(redirect.url("/example.whl"))
              .starts_with("Could not reach the download server ("));
}

namespace {

// Forwards to `inner` and cancels `cancel` once the body's first `bytes` have reached the
// download's sink: the cancel lands during the transfer however slowly the bytes arrive.
class CancelAfterBytes final : public fv::http::Client {
public:
    CancelAfterBytes(std::shared_ptr<fv::http::Client> inner, fv::CancelSource& cancel,
                     std::size_t bytes)
        : inner_(std::move(inner)), cancel_(cancel), bytes_(bytes) {}

    std::expected<fv::http::Response, fv::http::Error> send(const fv::http::Request& request,
                                                            const fv::CancelToken& token,
                                                            fv::http::BodySink* stream) override {
        Sink sink(stream, cancel_, bytes_);
        return inner_->send(request, token, stream != nullptr ? &sink : nullptr);
    }

private:
    class Sink final : public fv::http::BodySink {
    public:
        Sink(fv::http::BodySink* inner, fv::CancelSource& cancel, std::size_t bytes)
            : inner_(inner), cancel_(cancel), bytes_(bytes) {}
        bool on_head(const fv::http::Response& head) override { return inner_->on_head(head); }
        bool write(std::span<const char> data) override {
            const bool more = inner_->write(data);
            seen_ += data.size();
            if (seen_ >= bytes_) {
                cancel_.cancel();
            }
            return more;
        }

    private:
        fv::http::BodySink* inner_;
        fv::CancelSource& cancel_;
        std::size_t bytes_;
        std::size_t seen_ = 0;
    };

    std::shared_ptr<fv::http::Client> inner_;
    fv::CancelSource& cancel_;
    std::size_t bytes_;
};

} // namespace

TEST_CASE("cancel during a transfer keeps or discards the partial", "[verified_download]") {
    LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write("HTTP/1.1 200 OK\r\nContent-Length: 17\r\nConnection: close\r\n\r\n" +
                    kBody.substr(0, 8));
        reply.wait(std::chrono::seconds(30));
    });
    for (const bool keep : {true, false}) {
        CAPTURE(keep);
        const fv::testkit::TempDataRoot root;
        fv::CancelSource cancel;
        // Cancelled once the 8 bytes the server sends have arrived.
        const auto client = std::make_shared<CancelAfterBytes>(direct_client(), cancel, 8);
        CHECK_THROWS_AS(fv::download::download_verified(
                            *client, server.url("/example.whl"), root.path() / "example.whl", 17,
                            fv::sha256_hex(kBody),
                            [](std::string_view, std::int64_t, std::int64_t) {}, cancel.token(),
                            keep),
                        DownloadCanceled);
        if (keep) {
            CHECK(read_file(root.path() / "example.whl.part") == kBody.substr(0, 8));
        } else {
            CHECK_FALSE(fs::exists(root.path() / "example.whl.part"));
        }
    }
}

TEST_CASE("a pinned file downloads over HTTPS through the system proxy settings",
          "[verified_download][platform]") {
    // Reaches the internet, so it runs only when FIORIVOCE_TEST_DOWNLOAD_URL, _SIZE and _SHA256
    // name a file to fetch.
    const auto url = fv::testkit::get_env("FIORIVOCE_TEST_DOWNLOAD_URL");
    const auto size = fv::testkit::get_env("FIORIVOCE_TEST_DOWNLOAD_SIZE");
    const auto sha256 = fv::testkit::get_env("FIORIVOCE_TEST_DOWNLOAD_SHA256");
    if (!url || !size || !sha256) {
        SKIP("FIORIVOCE_TEST_DOWNLOAD_URL, _SIZE and _SHA256 are not set");
    }
    const fv::testkit::TempDataRoot root;
    const auto client = fv::http::make_curl_client();
    std::int64_t last = 0;
    fv::download::download_verified(
        *client, *url, root.path() / "pinned.bin", std::stoll(*size), *sha256,
        [&last](std::string_view, std::int64_t done, std::int64_t) { last = done; }, {});
    CHECK(last == std::stoll(*size));
    CHECK(fs::file_size(root.path() / "pinned.bin") == std::stoull(*size));
}
