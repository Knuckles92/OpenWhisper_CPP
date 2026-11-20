// Multipart bodies: the parts as httpx builds them, a file part streamed from disk by the curl
// client to a loopback server, a cancel while the body is still going out, and the fake client's
// record of what was uploaded.

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include "http/client.h"
#include "http/multipart.h"
#include "http/testing/fake_http_client.h"
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

void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string boundary_of(const std::string& content_type) {
    const auto at = content_type.find("boundary=");
    REQUIRE(at != std::string::npos);
    return content_type.substr(at + 9);
}

} // namespace

TEST_CASE("form fields and file parts are built as httpx builds them", "[multipart]") {
    const auto field = fv::http::form_field("model", "whisper-1");
    CHECK(field.name == "model");
    CHECK(field.filename.empty());
    CHECK(field.content_type.empty());
    CHECK(std::get<std::string>(field.data) == "whisper-1");

    const auto file =
        fv::http::file_part("file", std::filesystem::path("dir") / "clip.wav", "audio/wav");
    CHECK(file.filename == "clip.wav");
    CHECK(std::get<std::filesystem::path>(file.data) == std::filesystem::path("dir") / "clip.wav");
    CHECK(fv::http::file_part("file", "a.wav", "audio/wav", "other.wav").filename == "other.wav");

    CHECK(fv::http::content_type_for("a.WAV") == "audio/wav");
    CHECK(fv::http::content_type_for("a.mp3") == "audio/mpeg");
    CHECK(fv::http::content_type_for("a.m4a") == "audio/x-m4a");
    CHECK(fv::http::content_type_for("a.flac") == "audio/x-flac");
    CHECK(fv::http::content_type_for("a.bin") == "application/octet-stream");
}

TEST_CASE("encode_multipart lays out each part between boundaries", "[multipart]") {
    const fv::testkit::TempDataRoot root;
    const auto audio = root.path() / "a b.wav";
    write_file(audio, std::string("RIFF\0\x01", 6));
    fv::http::Multipart form{
        {fv::http::form_field("model", "whisper-1"), fv::http::form_field("keywords[]", "Ksenia"),
         fv::http::file_part("file", audio, "audio/wav"), fv::http::form_field("say \"hi\"", "x")}};
    CHECK(fv::http::encode_multipart(form, "XYZ") ==
          "--XYZ\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
          "--XYZ\r\nContent-Disposition: form-data; name=\"keywords[]\"\r\n\r\nKsenia\r\n"
          "--XYZ\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a b.wav\"\r\n"
          "Content-Type: audio/wav\r\n\r\n" +
              std::string("RIFF\0\x01", 6) +
              "\r\n--XYZ\r\nContent-Disposition: form-data; name=\"say %22hi%22\"\r\n\r\nx\r\n"
              "--XYZ--\r\n");
    form.parts.push_back(fv::http::file_part("file", root.path() / "missing.wav", "audio/wav"));
    CHECK_THROWS_AS(fv::http::encode_multipart(form, "XYZ"), std::runtime_error);
}

TEST_CASE("a multipart body streams its file part from disk", "[multipart][curl_client]") {
    LoopbackServer server([](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(200, "OK", {}, "uploaded"));
    });
    const fv::testkit::TempDataRoot root;
    // Larger than any single read curl makes, with every byte value.
    std::string content;
    for (int i = 0; i < 300'000; ++i) {
        content += static_cast<char>((i * 31) & 0xFF);
    }
    const auto audio = root.path() / u8"äudio.wav";
    write_file(audio, content);

    fv::http::Request request;
    request.method = "POST";
    request.url = server.url("/v1/audio/transcriptions");
    fv::http::Multipart form{{fv::http::form_field("model", "whisper-1"),
                              fv::http::form_field("response_format", "text"),
                              fv::http::file_part("file", audio, "audio/wav")}};
    request.body = form;
    const auto response = direct_client()->send(request, {});
    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->body == "uploaded");

    const auto seen = server.requests();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].method == "POST");
    const auto content_type = seen[0].header("Content-Type");
    REQUIRE(content_type);
    CHECK(content_type->starts_with("multipart/form-data; boundary="));
    // The bytes on the wire are exactly the encoded form, file streamed in place.
    CHECK(seen[0].body == fv::http::encode_multipart(form, boundary_of(*content_type)));
    CHECK(seen[0].header("Content-Length") == std::to_string(seen[0].body.size()));
}

TEST_CASE("cancel ends an upload while its body is still going out", "[multipart][curl_client]") {
    // A server that accepts the connection and never reads: the upload stalls once the socket
    // buffers fill, and only the cancel can end it.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    boost::asio::ip::tcp::socket held(io);
    std::thread accepting([&] {
        boost::system::error_code ignored;
        acceptor.accept(held, ignored);
    });

    const fv::testkit::TempDataRoot root;
    const auto audio = root.path() / "large.wav";
    write_file(audio, std::string(64u << 20, '\x5a'));
    fv::http::Request request;
    request.method = "POST";
    request.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/audio/transcriptions";
    request.body = fv::http::Multipart{{fv::http::file_part("file", audio, "audio/wav")}};

    fv::CancelSource cancel;
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(500ms);
        cancel.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto response = direct_client()->send(request, cancel.token());
    canceller.join();
    accepting.join();
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == fv::http::ErrorKind::Cancelled);
    CHECK(std::chrono::steady_clock::now() - started < 10s);
}

TEST_CASE("the fake client records what each multipart request uploaded", "[multipart]") {
    const fv::testkit::TempDataRoot root;
    const auto audio = root.path() / "chunk_000.wav";
    write_file(audio, "RIFF-bytes");
    fv::http::FakeHttpClient fake;
    fake.push({});
    fake.push({});
    fv::http::Request plain;
    plain.url = "http://127.0.0.1:1/";
    REQUIRE(fake.send(plain, {}, nullptr).has_value());
    fv::http::Request upload = plain;
    upload.method = "POST";
    upload.body = fv::http::Multipart{
        {fv::http::form_field("model", "m"), fv::http::file_part("file", audio, "audio/wav")}};
    REQUIRE(fake.send(upload, {}, nullptr).has_value());
    // The chunk is gone, as the backend removes it; the record still has its bytes.
    std::filesystem::remove(audio);
    const auto parts = fake.multipart_parts();
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].empty());
    REQUIRE(parts[1].size() == 2);
    CHECK(std::get<std::string>(parts[1][0].data) == "m");
    CHECK(parts[1][1].filename == "chunk_000.wav");
    CHECK(std::get<std::string>(parts[1][1].data) == "RIFF-bytes");

    // A file part that cannot be read fails before anything is answered, as with curl.
    const auto missing = fake.send(upload, {}, nullptr);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind == fv::http::ErrorKind::Protocol);
    CHECK(fake.pending() == 0);
}
