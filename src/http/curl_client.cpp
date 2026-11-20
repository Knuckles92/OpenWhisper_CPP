#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>

#include "base/log.h"
#include "http/client.h"
#include "http/detail/curl_mime.h"
#include "http/detail/os_text.h"
#include "http/proxy.h"

namespace fv::http {

namespace {

// httpx logs every request at INFO under this name; install_app_logging holds it at WARNING.
constexpr log::Logger kLog("httpx");

// urllib.request's HTTPRedirectHandler.max_redirections.
constexpr long kMaxRedirects = 10;

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return ascii_lower(x) == ascii_lower(y);
           });
}

void global_init() {
    static std::once_flag once;
    // Never cleaned up: other threads may still hold easy handles when the process exits.
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// One share handle for every request of a client: DNS cache, TLS sessions and live connections.
class Share {
public:
    Share() : handle_(curl_share_init()) {
        curl_share_setopt(handle_, CURLSHOPT_LOCKFUNC, &Share::lock);
        curl_share_setopt(handle_, CURLSHOPT_UNLOCKFUNC, &Share::unlock);
        curl_share_setopt(handle_, CURLSHOPT_USERDATA, this);
        curl_share_setopt(handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    }
    ~Share() { curl_share_cleanup(handle_); }
    Share(const Share&) = delete;
    Share& operator=(const Share&) = delete;

    CURLSH* get() const { return handle_; }

private:
    static void lock(CURL*, curl_lock_data data, curl_lock_access, void* self) {
        static_cast<Share*>(self)->mutexes_[static_cast<std::size_t>(data)].lock();
    }
    static void unlock(CURL*, curl_lock_data data, void* self) {
        static_cast<Share*>(self)->mutexes_[static_cast<std::size_t>(data)].unlock();
    }

    CURLSH* handle_;
    std::array<std::mutex, CURL_LOCK_DATA_LAST> mutexes_;
};

struct EasyDeleter {
    void operator()(CURL* easy) const { curl_easy_cleanup(easy); }
};
struct ListDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};
struct UrlDeleter {
    void operator()(CURLU* url) const { curl_url_cleanup(url); }
};

// What one transfer has seen so far. The callbacks never throw into curl: an exception from a
// sink is kept and rethrown once curl_easy_perform has returned.
struct Transfer {
    CURL* easy = nullptr;
    const Request* request = nullptr;
    const CancelToken* cancel = nullptr;
    BodySink* sink = nullptr;

    Response head;
    std::string protocol; // "HTTP/1.1" of the last status line
    // The connection to the current hop is up and the request is going out on it.
    bool request_sent = false;
    // Times the request went out, redirect hops included: curl sends it once more on a fresh
    // connection by itself when a reused one died before anything was answered.
    int sends = 0;
    bool head_complete = false;
    bool head_delivered = false;
    bool sink_stopped = false;
    bool too_large = false;
    std::exception_ptr failure;
    std::ifstream upload;

    bool deliver_head() {
        head_delivered = true;
        if (sink == nullptr) {
            return true;
        }
        char* url = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url != nullptr) {
            head.effective_url = url;
        }
        if (!sink->on_head(head)) {
            sink_stopped = true;
            return false;
        }
        return true;
    }
};

std::string_view trim_line_end(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    return line;
}

// http.client's status line: version, status and the rest of the line stripped as the reason.
void parse_status_line(std::string_view line, Transfer& transfer) {
    const auto first = line.find(' ');
    transfer.protocol = std::string(line.substr(0, first));
    transfer.head.status = 0;
    transfer.head.reason.clear();
    if (first == std::string_view::npos) {
        return;
    }
    auto rest = line.substr(first + 1);
    while (rest.starts_with(' ')) {
        rest.remove_prefix(1);
    }
    const auto second = rest.find(' ');
    const auto code = rest.substr(0, second);
    int status = 0;
    for (const char c : code) {
        if (c < '0' || c > '9') {
            status = 0;
            break;
        }
        status = status * 10 + (c - '0');
    }
    transfer.head.status = status;
    if (second != std::string_view::npos) {
        auto reason = rest.substr(second + 1);
        while (!reason.empty() && (reason.front() == ' ' || reason.front() == '\t')) {
            reason.remove_prefix(1);
        }
        while (!reason.empty() && (reason.back() == ' ' || reason.back() == '\t')) {
            reason.remove_suffix(1);
        }
        transfer.head.reason = std::string(reason);
    }
}

std::size_t on_header(char* buffer, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    const std::string_view raw(buffer, bytes);
    const auto line = trim_line_end(raw);
    if (line.starts_with("HTTP/")) {
        // Each response of a redirect chain (and a 100 Continue) starts a new head.
        transfer.head.headers.clear();
        transfer.head_complete = false;
        parse_status_line(line, transfer);
    } else if (line.empty()) {
        const int status = transfer.head.status;
        if (transfer.request->follow_redirects && status >= 300 && status < 400 && status != 304 &&
            find_header(transfer.head.headers, "Location")) {
            // curl goes on to the next hop, which urllib opens as a new request: a failure to
            // reach it is a URLError again.
            transfer.request_sent = false;
            transfer.head_complete = false;
        } else {
            transfer.head_complete = true;
        }
    } else if ((line.front() == ' ' || line.front() == '\t') && !transfer.head.headers.empty()) {
        // A folded line continues the previous value, kept as email's compat32 policy keeps it.
        transfer.head.headers.back().second += "\r\n";
        transfer.head.headers.back().second += line;
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        transfer.head.headers.emplace_back(std::string(line.substr(0, colon)), std::string(value));
    }
    return bytes;
}

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    try {
        if (!transfer.head_delivered && !transfer.deliver_head()) {
            return 0;
        }
        if (transfer.sink != nullptr) {
            if (!transfer.sink->write(std::span<const char>(data, bytes))) {
                transfer.sink_stopped = true;
                return 0;
            }
            return bytes;
        }
        if (transfer.head.body.size() + bytes > transfer.request->max_body_bytes) {
            transfer.too_large = true;
            return 0;
        }
        transfer.head.body.append(data, bytes);
        return bytes;
    } catch (...) {
        transfer.failure = std::current_exception();
        transfer.sink_stopped = true;
        return 0;
    }
}

std::size_t on_upload(char* buffer, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<Transfer*>(user);
    transfer.upload.read(buffer, static_cast<std::streamsize>(size * count));
    if (transfer.upload.bad()) {
        return CURL_READFUNC_ABORT;
    }
    return static_cast<std::size_t>(transfer.upload.gcount());
}

// Called once the connection (new or reused, any proxy tunnel and TLS handshake included) is
// ready and the request is about to be sent.
int on_prereq(void* user, char*, char*, int, int) {
    auto& transfer = *static_cast<Transfer*>(user);
    transfer.request_sent = true;
    ++transfer.sends;
    return CURL_PREREQFUNC_OK;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& transfer = *static_cast<Transfer*>(user);
    return transfer.cancel->cancelled() ? 1 : 0;
}

std::string url_for_log(std::string_view url) {
    return std::string(url.substr(0, url.find_first_of("?#")));
}

Error make_error(ErrorKind kind, std::string message) {
    return Error{kind, std::move(message)};
}

Error classify(CURLcode code, CURL* easy, const char* buffer) {
    long os_error = 0;
    curl_easy_getinfo(easy, CURLINFO_OS_ERRNO, &os_error);
    const std::string text = (buffer != nullptr && buffer[0] != '\0')
                                 ? std::string(buffer)
                                 : std::string(curl_easy_strerror(code));
    const auto socket_text = [&] {
        return os_error != 0 ? detail::socket_error_text(os_error) : text;
    };
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        return make_error(ErrorKind::Timeout, "timed out");
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return make_error(ErrorKind::Dns, detail::dns_failure_text());
    case CURLE_COULDNT_CONNECT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return make_error(ErrorKind::Connect, socket_text());
    case CURLE_GOT_NOTHING:
        // http.client.RemoteDisconnected
        return make_error(ErrorKind::Connect, "Remote end closed connection without response");
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_INVALIDCERTSTATUS: {
        Error error = make_error(ErrorKind::Tls, text);
        error.certificate = true;
        return error;
    }
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_SHUTDOWN_FAILED:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_CLIENTCERT:
        return make_error(ErrorKind::Tls, text);
    case CURLE_ABORTED_BY_CALLBACK:
        return make_error(ErrorKind::Cancelled, "canceled");
    case CURLE_PARTIAL_FILE: {
        Error error = make_error(ErrorKind::Protocol, text);
        error.truncated = true;
        return error;
    }
    default:
        return make_error(ErrorKind::Protocol, text);
    }
}

class CurlClient final : public Client {
public:
    explicit CurlClient(CurlClientOptions options) : options_(std::move(options)) {}

    std::expected<Response, Error> send(const Request& request, const CancelToken& cancel,
                                        BodySink* stream) override {
        if (cancel.cancelled()) {
            return std::unexpected(make_error(ErrorKind::Cancelled, "canceled"));
        }
        std::unique_ptr<CURL, EasyDeleter> owner(curl_easy_init());
        CURL* easy = owner.get();
        if (easy == nullptr) {
            return std::unexpected(make_error(ErrorKind::Protocol, "curl_easy_init failed"));
        }
        Transfer transfer;
        transfer.easy = easy;
        transfer.request = &request;
        transfer.cancel = &cancel;
        transfer.sink = stream;

        std::array<char, CURL_ERROR_SIZE> error_buffer{};
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer.data());
        curl_easy_setopt(easy, CURLOPT_SHARE, share_.get());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, kMaxRedirects);
        // Python's ssl module does no revocation check; failing only on a definite "revoked"
        // keeps downloads working where OpenWhisper's did, behind proxies that block CRLs.
        curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS,
                         static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
        curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);

        curl_easy_setopt(
            easy, CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<long>(std::max<std::int64_t>(1, request.connect_timeout.count())));
        // "Idle" is no byte at all for that long: under 1 byte/s across the whole window.
        const auto idle_seconds =
            std::max<std::int64_t>(1, (request.idle_timeout.count() + 999) / 1000);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, static_cast<long>(idle_seconds));
        if (request.deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                *request.deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return std::unexpected(make_error(ErrorKind::Timeout, "timed out"));
            }
            curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(left.count()));
        }

        // The proxy is decided here, never by curl's own reading of the environment.
        std::string proxy;
        if (!request.bypass_proxy) {
            const auto chosen = options_.proxy
                                    ? options_.proxy(request.url)
                                    : proxy_for_url(request.url, read_system_proxy_settings());
            proxy = chosen.value_or("");
        }
        curl_easy_setopt(easy, CURLOPT_PROXY, proxy.c_str());
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "");

        std::unique_ptr<curl_slist, ListDeleter> resolve;
        if (request.pinned_address) {
            std::unique_ptr<CURLU, UrlDeleter> parsed(curl_url());
            char* host = nullptr;
            char* port = nullptr;
            if (curl_url_set(parsed.get(), CURLUPART_URL, request.url.c_str(), 0) != CURLUE_OK ||
                curl_url_get(parsed.get(), CURLUPART_HOST, &host, 0) != CURLUE_OK ||
                curl_url_get(parsed.get(), CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) !=
                    CURLUE_OK) {
                curl_free(host);
                curl_free(port);
                return std::unexpected(make_error(ErrorKind::Protocol, "invalid URL"));
            }
            std::string address = *request.pinned_address;
            if (address.find(':') != std::string::npos && !address.starts_with('[')) {
                address = "[" + address + "]";
            }
            const std::string entry = std::string(host) + ":" + port + ":" + address;
            curl_free(host);
            curl_free(port);
            resolve.reset(curl_slist_append(nullptr, entry.c_str()));
            curl_easy_setopt(easy, CURLOPT_RESOLVE, resolve.get());
        }

        std::unique_ptr<curl_slist, ListDeleter> headers;
        const auto add_header = [&headers](const std::string& line) {
            curl_slist* grown = curl_slist_append(headers.get(), line.c_str());
            if (grown != nullptr) {
                headers.release();
                headers.reset(grown);
            }
        };
        for (const auto& [name, value] : request.headers) {
            // "Name;" is curl's spelling of a header sent with an empty value.
            add_header(value.empty() ? name + ";" : name + ": " + value);
        }
        // No "Expect: 100-continue" pause before large bodies; neither urllib nor httpx sends it.
        add_header("Expect:");
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get());
        if (request.range_from) {
            const std::string range = std::to_string(*request.range_from) + "-";
            curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());
        }

        // Freed after curl_easy_perform has returned (declared after `owner`, so before it).
        detail::MimePtr mime;
        if (const auto* multipart = std::get_if<Multipart>(&request.body)) {
            auto built = detail::make_curl_mime(easy, *multipart);
            if (!built) {
                return std::unexpected(make_error(ErrorKind::Protocol, std::move(built.error())));
            }
            mime = std::move(*built);
            curl_easy_setopt(easy, CURLOPT_MIMEPOST, mime.get());
        } else if (const auto* bytes = std::get_if<std::string>(&request.body)) {
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(bytes->size()));
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, bytes->data());
        } else if (const auto* file = std::get_if<std::filesystem::path>(&request.body)) {
            std::error_code ignored;
            const auto size = std::filesystem::file_size(*file, ignored);
            transfer.upload.open(*file, std::ios::binary);
            if (!transfer.upload || ignored) {
                return std::unexpected(
                    make_error(ErrorKind::Protocol, "the request body file could not be read"));
            }
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
            curl_easy_setopt(easy, CURLOPT_READFUNCTION, &on_upload);
            curl_easy_setopt(easy, CURLOPT_READDATA, &transfer);
        }
        if (request.method == "HEAD") {
            curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        } else if (request.method == "GET" &&
                   std::holds_alternative<std::monostate>(request.body)) {
            curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
        } else if (request.method != "POST") {
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }

        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &on_header);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &on_body);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(easy, CURLOPT_PREREQFUNCTION, &on_prereq);
        curl_easy_setopt(easy, CURLOPT_PREREQDATA, &transfer);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &on_progress);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &transfer);

        const CURLcode code = curl_easy_perform(easy);
        if (transfer.failure) {
            std::rethrow_exception(transfer.failure);
        }

        if (code == CURLE_OK || (code == CURLE_WRITE_ERROR && transfer.sink_stopped)) {
            long status = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
            transfer.head.status = static_cast<int>(status);
            char* url = nullptr;
            if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK &&
                url != nullptr) {
                transfer.head.effective_url = url;
            }
            if (!transfer.head_delivered) {
                transfer.deliver_head();
                if (transfer.failure) {
                    std::rethrow_exception(transfer.failure);
                }
            }
            if (request.log_request) {
                kLog.info("HTTP Request: {} {} \"{} {} {}\"", request.method,
                          url_for_log(request.url), transfer.protocol, transfer.head.status,
                          transfer.head.reason);
            }
            return std::move(transfer.head);
        }
        if (transfer.too_large) {
            return std::unexpected(make_error(ErrorKind::TooLarge, "the response is too large"));
        }
        Error error = classify(code, easy, error_buffer.data());
        error.after_head = transfer.head_complete;
        // A failure to send the request is still part of urllib's h.request(), so a URLError.
        error.after_request =
            transfer.head_complete || (transfer.request_sent && code != CURLE_SEND_ERROR);
        error.sends = transfer.sends;
        return std::unexpected(std::move(error));
    }

private:
    CurlClientOptions options_;
    Share share_;
};

} // namespace

std::optional<std::string> find_header(const Headers& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (iequals(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

std::shared_ptr<Client> make_curl_client() {
    return make_curl_client(CurlClientOptions{});
}

std::shared_ptr<Client> make_curl_client(CurlClientOptions options) {
    global_init();
    return std::make_shared<CurlClient>(std::move(options));
}

} // namespace fv::http
