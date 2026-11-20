#pragma once

// What the hook engine tests share: waiting, a closed port, a server that hangs up, a log
// capture and the hook child's path. Everything talks to 127.0.0.1 only.

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

// <windows.h>, which Asio brings in, defines these as empty macros; golden::near is a function.
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/log.h"
#include "base/utf8.h"
#include "http/client.h"
#include "process/launcher.h"

namespace fv::automation::test {

// Polls every 10 ms; false when the time ran out.
inline bool wait_until(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

inline void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

inline double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

inline double seconds_between(std::chrono::steady_clock::time_point from,
                              std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

// A port on 127.0.0.1 nothing listens on (bound once, then closed).
inline std::uint16_t closed_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    return acceptor.local_endpoint().port();
}

// A server that accepts each connection and closes it without answering.
class HangUpServer {
public:
    HangUpServer()
        : acceptor_(io_,
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] {
            while (true) {
                boost::asio::ip::tcp::socket socket(io_);
                boost::system::error_code error;
                acceptor_.accept(socket, error);
                if (stopping_.load()) {
                    return;
                }
                if (!error) {
                    accepted_.fetch_add(1);
                    socket.close(error);
                }
            }
        });
    }
    ~HangUpServer() {
        stopping_.store(true);
        // Wakes the blocking accept with a connection of our own.
        try {
            boost::asio::io_context io;
            boost::asio::ip::tcp::socket socket(io);
            socket.connect(acceptor_.local_endpoint());
        } catch (...) {
        }
        thread_.join();
    }
    HangUpServer(const HangUpServer&) = delete;
    HangUpServer& operator=(const HangUpServer&) = delete;

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }
    int accepted() const { return accepted_.load(); }

private:
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
    std::thread thread_;
};

// Every record, DEBUG included, for the scope. "httpx" stays at WARNING, as install_app_logging
// holds it in the app.
class LogCapture final : public log::Sink {
public:
    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        text_ += std::string(record.logger) + " " + std::string(record.message) + "\n";
    }
    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
};

class ScopedLogCapture {
public:
    ScopedLogCapture() : capture_(std::make_shared<LogCapture>()), previous_(log::level()) {
        log::set_level(log::Level::Debug);
        log::set_logger_level("httpx", log::Level::Warning);
        log::add_sink(capture_);
    }
    ~ScopedLogCapture() {
        log::remove_sink(capture_);
        log::set_logger_level("httpx", std::nullopt);
        log::set_level(previous_);
    }
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    std::string text() const { return capture_->text(); }

private:
    std::shared_ptr<LogCapture> capture_;
    log::Level previous_;
};

// fv_automation_hook_child, the program the [platform] tests start (commands in its header
// comment).
inline std::string hook_child() {
    return FV_HOOK_CHILD;
}

// First line of a test that needs hook_child to run. Programs start on Windows only (the posix
// launcher is a stub), so elsewhere the test skips instead of failing.
inline void require_program_start() {
    if (!can_start_programs()) {
        SKIP("Starting programs is not supported on this platform");
    }
}

// A fresh folder under the system's temporary folder for one test, removed afterwards.
class TempFolder {
public:
    TempFolder() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("fv-hooks-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string file(std::string_view name) const { return path_to_utf8(path_ / name); }

private:
    std::filesystem::path path_;
};

inline std::string read_text(const std::string& file) {
    std::ifstream in(path_from_utf8(file), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

inline std::vector<std::string> read_lines(const std::string& file) {
    std::vector<std::string> lines;
    std::istringstream in(read_text(file));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

// A client that reaches 127.0.0.1 directly, whatever the machine's proxy settings say.
inline std::shared_ptr<http::Client> loopback_client() {
    return http::make_curl_client(
        http::CurlClientOptions{.proxy = [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        }});
}

} // namespace fv::automation::test
