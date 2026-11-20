#include "http_client.h"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <cctype>
#include <format>

#include "pyport/json.h"

namespace fv::meeting_web::test_support {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

// A chunked body: hex size lines, the chunks, a zero chunk.
std::string unchunk(std::string_view raw) {
    std::string body;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t line_end = raw.find("\r\n", pos);
        REQUIRE(line_end != std::string_view::npos);
        const std::size_t size =
            std::stoull(std::string(raw.substr(pos, line_end - pos)), nullptr, 16);
        pos = line_end + 2;
        if (size == 0) {
            break;
        }
        body.append(raw.substr(pos, size));
        pos += size + 2;
    }
    return body;
}

} // namespace

std::optional<std::string> HttpResult::header(std::string_view name) const {
    const std::string wanted = lower(name);
    for (const auto& [key, value] : headers) {
        if (key == wanted) {
            return value;
        }
    }
    return std::nullopt;
}

Json HttpResult::json() const {
    return py::loads(body);
}

std::string percent_encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        }
    }
    return out;
}

std::string query_string(const std::vector<std::pair<std::string, std::string>>& query) {
    std::string out;
    for (const auto& [name, value] : query) {
        if (!out.empty()) {
            out += '&';
        }
        out += percent_encode(name) + "=" + percent_encode(value);
    }
    return out;
}

HttpResult http_request(std::uint16_t port, std::string_view method, std::string_view target,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        std::string_view body) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    std::string request = std::format(
        "{} {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nConnection: close\r\n", method, target, port);
    for (const auto& [name, value] : headers) {
        request += name + ": " + value + "\r\n";
    }
    if (!body.empty() || method == "POST") {
        request += std::format("Content-Length: {}\r\n", body.size());
    }
    request += "\r\n";
    request += body;
    boost::asio::write(socket, boost::asio::buffer(request));

    std::string raw;
    boost::system::error_code error;
    char buffer[16384];
    for (;;) {
        const std::size_t read = socket.read_some(boost::asio::buffer(buffer), error);
        raw.append(buffer, read);
        if (error) {
            break;
        }
    }
    REQUIRE((error == boost::asio::error::eof || error == boost::asio::error::connection_reset));

    HttpResult result;
    const std::size_t head_end = raw.find("\r\n\r\n");
    REQUIRE(head_end != std::string::npos);
    const std::string head = raw.substr(0, head_end);
    std::size_t line_end = head.find("\r\n");
    const std::string status_line = head.substr(0, line_end);
    REQUIRE(status_line.starts_with("HTTP/1.1 "));
    result.status = std::stoi(status_line.substr(9, 3));
    while (line_end != std::string::npos) {
        const std::size_t start = line_end + 2;
        line_end = head.find("\r\n", start);
        const std::string line = head.substr(
            start, line_end == std::string::npos ? std::string::npos : line_end - start);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        result.headers.emplace_back(lower(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    std::string rest = raw.substr(head_end + 4);
    if (result.header("transfer-encoding") == "chunked") {
        result.body = unchunk(rest);
    } else if (const auto length = result.header("content-length")) {
        const auto size = std::stoull(*length);
        REQUIRE(rest.size() >= size);
        result.body = rest.substr(0, size);
    } else {
        result.body = std::move(rest);
    }
    return result;
}

} // namespace fv::meeting_web::test_support
