#pragma once

// A small HTTP/1.1 server on 127.0.0.1 for tests of the HTTP client and of downloads: each
// connection runs a scripted handler on its own thread. With `tls`, it speaks HTTPS with a
// self-signed certificate made for the run, which no client trusts. Nothing here leaves the
// loopback interface.

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

// <windows.h>, which Asio brings in, defines these as empty macros; golden::near is a function.
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fv::testing {

class LoopbackServer {
public:
    struct Request {
        std::string method;
        std::string target; // as on the request line: a path, or a full URL through a proxy
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;

        std::optional<std::string> header(std::string_view name) const {
            for (const auto& [key, value] : headers) {
                if (key.size() == name.size() &&
                    std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                    })) {
                    return value;
                }
            }
            return std::nullopt;
        }
    };

    // What a handler writes to. The connection closes after the handler returns.
    class Reply {
    public:
        virtual ~Reply() = default;
        virtual void write(std::string_view bytes) = 0;
        // Sleeps, waking early when the server stops; false then.
        virtual bool wait(std::chrono::milliseconds) = 0;
    };

    using Handler = std::function<void(const Request&, Reply&)>;

    // A whole response: status line, the headers given, Content-Length unless given, and
    // "Connection: close".
    static std::string response(int status, std::string_view reason,
                                std::vector<std::pair<std::string, std::string>> headers,
                                std::string_view body) {
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
        bool has_length = false;
        for (const auto& [name, value] : headers) {
            out += name + ": " + value + "\r\n";
            has_length = has_length || name == "Content-Length";
        }
        if (!has_length) {
            out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        out += "Connection: close\r\n\r\n";
        out += body;
        return out;
    }

    explicit LoopbackServer(Handler handler, bool tls = false)
        : handler_(std::move(handler)), tls_(tls),
          acceptor_(io_,
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)),
          ssl_(boost::asio::ssl::context::tls_server) {
        if (tls_) {
            make_certificate();
        }
        port_ = acceptor_.local_endpoint().port();
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~LoopbackServer() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        // Wakes the blocking accept with a connection of our own.
        try {
            boost::asio::io_context io;
            boost::asio::ip::tcp::socket socket(io);
            socket.connect(acceptor_.local_endpoint());
        } catch (...) {
        }
        accept_thread_.join();
        std::list<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            workers.swap(workers_);
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    std::uint16_t port() const { return port_; }
    std::string url(std::string_view path = "/") const {
        return std::string(tls_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_) +
               std::string(path);
    }
    std::vector<Request> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    template <class Stream> class StreamReply final : public Reply {
    public:
        StreamReply(Stream& stream, LoopbackServer& server) : stream_(stream), server_(server) {}
        void write(std::string_view bytes) override {
            boost::system::error_code ignored;
            boost::asio::write(stream_, boost::asio::buffer(bytes.data(), bytes.size()), ignored);
        }
        bool wait(std::chrono::milliseconds duration) override {
            std::unique_lock lock(server_.mutex_);
            return !server_.wake_.wait_for(lock, duration, [this] { return server_.stopping_; });
        }

    private:
        Stream& stream_;
        LoopbackServer& server_;
    };

    void make_certificate() {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* certificate = X509_new();
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
        X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
        X509_set_pubkey(certificate, key);
        X509_NAME* name = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
        X509_set_issuer_name(certificate, name);
        X509_sign(certificate, key, EVP_sha256());
        SSL_CTX_use_certificate(ssl_.native_handle(), certificate);
        SSL_CTX_use_PrivateKey(ssl_.native_handle(), key);
        X509_free(certificate);
        EVP_PKEY_free(key);
    }

    void accept_loop() {
        while (true) {
            boost::asio::ip::tcp::socket socket(io_);
            boost::system::error_code error;
            acceptor_.accept(socket, error);
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            if (error) {
                continue;
            }
            workers_.emplace_back(
                [this, connection = std::make_shared<boost::asio::ip::tcp::socket>(
                           std::move(socket))]() mutable { serve(*connection); });
        }
    }

    template <class Stream> std::optional<Request> read_request(Stream& stream) {
        boost::asio::streambuf buffer;
        boost::system::error_code error;
        boost::asio::read_until(stream, buffer, "\r\n\r\n", error);
        if (error && buffer.size() == 0) {
            return std::nullopt;
        }
        std::string data(boost::asio::buffers_begin(buffer.data()),
                         boost::asio::buffers_end(buffer.data()));
        const auto end = data.find("\r\n\r\n");
        if (end == std::string::npos) {
            return std::nullopt;
        }
        Request request;
        std::string_view head(data.data(), end);
        const auto line_end = head.find("\r\n");
        const auto line = head.substr(0, line_end);
        const auto first = line.find(' ');
        const auto second = line.find(' ', first + 1);
        request.method = std::string(line.substr(0, first));
        request.target = std::string(line.substr(first + 1, second - first - 1));
        auto rest =
            line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);
        while (!rest.empty()) {
            const auto next = rest.find("\r\n");
            const auto header = rest.substr(0, next);
            const auto colon = header.find(':');
            if (colon != std::string_view::npos) {
                auto value = header.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') {
                    value.remove_prefix(1);
                }
                request.headers.emplace_back(std::string(header.substr(0, colon)),
                                             std::string(value));
            }
            rest = next == std::string_view::npos ? std::string_view() : rest.substr(next + 2);
        }
        request.body = data.substr(end + 4);
        if (const auto length = request.header("Content-Length")) {
            const auto wanted = static_cast<std::size_t>(std::stoull(*length));
            if (request.body.size() < wanted) {
                std::string more(wanted - request.body.size(), '\0');
                boost::asio::read(stream, boost::asio::buffer(more), error);
                request.body += more;
            }
        }
        return request;
    }

    template <class Stream> void serve_stream(Stream& stream) {
        const auto request = read_request(stream);
        if (!request) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(*request);
        }
        StreamReply<Stream> reply(stream, *this);
        handler_(*request, reply);
    }

    void serve(boost::asio::ip::tcp::socket& socket) {
        try {
            if (tls_) {
                boost::asio::ssl::stream<boost::asio::ip::tcp::socket&> stream(socket, ssl_);
                boost::system::error_code error;
                stream.handshake(boost::asio::ssl::stream_base::server, error);
                if (error) {
                    return;
                }
                serve_stream(stream);
                stream.shutdown(error);
            } else {
                serve_stream(socket);
            }
            boost::system::error_code ignored;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            socket.close(ignored);
        } catch (...) {
            // A client that went away mid-reply ends only its own connection.
        }
    }

    Handler handler_;
    bool tls_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context ssl_;
    std::uint16_t port_ = 0;
    std::thread accept_thread_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::list<std::thread> workers_;
    std::vector<Request> requests_;
};

} // namespace fv::testing
