#include "web_test_support.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace fv::meeting_web::test_support {

Json untag(const Json& value) {
    if (value.is_object()) {
        if (value.size() == 1 && value.contains("$float")) {
            const auto tag = value.at("$float").get<std::string>();
            if (tag == "nan") {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return tag == "inf" ? std::numeric_limits<double>::infinity()
                                : -std::numeric_limits<double>::infinity();
        }
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = untag(item);
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(untag(item));
        }
        return out;
    }
    return value;
}

Client::Client(std::uint16_t port, std::string_view query) {
    const std::string path = query.empty() ? "/ws" : "/ws?" + std::string(query);
    INFO("connecting to " << path);
    REQUIRE(client_.connect(port, path));
}

Json Client::receive() {
    return py::loads(receive_text());
}

std::string Client::receive_text() {
    auto message = client_.receive(kWait);
    REQUIRE(message.has_value());
    REQUIRE_FALSE(message->binary);
    return std::move(message->data);
}

Json Client::receive_until(std::string_view type, int limit) {
    for (int i = 0; i < limit; ++i) {
        Json message = receive();
        if (message.value("type", std::string()) == type) {
            return message;
        }
    }
    FAIL("No '" << type << "' message within " << limit << " messages");
    return {};
}

std::optional<Json> Client::try_receive(std::chrono::milliseconds wait) {
    auto message = client_.receive(wait);
    if (!message) {
        return std::nullopt;
    }
    return py::loads(message->data);
}

void Client::send(const Json& message) {
    client_.send_text(py::dumps(message));
}

void Client::send_text(std::string_view text) {
    client_.send_text(text);
}

void Client::send_binary(std::string_view bytes) {
    client_.send_binary(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

std::optional<std::uint16_t> Client::closed_with(std::chrono::milliseconds wait) {
    // Frames the server sent before closing are skipped.
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (client_.close_code()) {
            return client_.close_code();
        }
        if (!client_.receive(std::chrono::milliseconds(200)) && client_.close_code()) {
            return client_.close_code();
        }
    }
    return client_.close_code();
}

void Client::close() {
    client_.close(1000);
}

} // namespace fv::meeting_web::test_support
