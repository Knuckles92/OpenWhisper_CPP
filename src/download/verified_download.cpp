#include "download/verified_download.h"

#include <algorithm>
#include <exception>
#include <regex>
#include <span>
#include <system_error>
#include <utility>

#include "base/hash.h"
#include "base/log.h"
#include "download/detail/format_size.h"
#include "download/detail/platform.h"
#include "download/detail/raw_file.h"
#include "download/verified_files.h"
#include "http/user_agent.h"

namespace fv::download {

namespace {

constexpr log::Logger kLog("services.verified_download");

constexpr int kPartialContent = 206;
constexpr int kRangeNotSatisfiable = 416;

// A network or disk failure inside the transfer, carried out to download_verified, which words
// it (Python's `except (URLError, OSError)`).
struct NetworkFailed {
    NetworkFailure failure;
};

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

bool is_python_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// int(text) for the ASCII forms a Content-Length can take: surrounding white space, a sign,
// and single underscores between digits. nullopt where Python raises ValueError.
std::optional<std::int64_t> python_int(std::string_view text) {
    while (!text.empty() && is_python_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_python_space(text.back())) {
        text.remove_suffix(1);
    }
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    bool previous_digit = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_') {
            if (!previous_digit || i + 1 == text.size()) {
                return std::nullopt;
            }
            previous_digit = false;
            continue;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        if (value > (INT64_MAX - (c - '0')) / 10) {
            // Larger than any download; compares unequal to every expected length.
            return negative ? INT64_MIN : INT64_MAX;
        }
        value = value * 10 + (c - '0');
        previous_digit = true;
    }
    return negative ? -value : value;
}

// The `.part` file of one download and the SHA-256 of its bytes. The digest covers every byte in
// the file, including any kept from an earlier attempt, so the final check is over the whole
// download.
class PartialFile {
public:
    PartialFile(detail::RawFile& out, std::string_view sha256_hex, std::int64_t size_bytes,
                const CancelToken& cancel, const Messages& text)
        : out_(out), expected_(lowercase(sha256_hex)), size_(size_bytes), cancel_(cancel),
          text_(text) {}

    std::int64_t resume_from() const { return resume_from_; }
    std::int64_t position() const { return position_; }

    void check_canceled() const {
        if (cancel_.cancelled()) {
            throw DownloadCanceled(text_.canceled);
        }
    }

    bool digest_matches() const { return digest_.hexdigest() == expected_; }

    // Drops everything on disk so the download starts again from byte 0.
    void restart() {
        resume_from_ = 0;
        digest_ = Sha256();
        out_.seek(0);
        position_ = 0;
        out_.truncate(0);
    }

    // Checks the file is safe to use and hashes what an earlier attempt left.
    void load_existing() {
        const auto info = out_.info();
        if (!info.regular || info.links != 1) {
            throw DownloadError(text_.partial_not_regular);
        }
        out_.make_private();

        resume_from_ = static_cast<std::int64_t>(info.size);
        if (resume_from_ > size_) {
            restart();
        }
        digest_ = Sha256();
        out_.seek(0);
        position_ = 0;
        std::vector<char> block(kChunkBytes);
        auto remaining = static_cast<std::uint64_t>(resume_from_);
        while (remaining > 0) {
            check_canceled();
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, remaining));
            const auto got = out_.read(std::span<char>(block.data(), want));
            if (got == 0) {
                throw DownloadError(text_.partial_unreadable);
            }
            digest_.update(std::string_view(block.data(), got));
            remaining -= got;
            position_ += static_cast<std::int64_t>(got);
        }
    }

    // _check_response, for a 2xx head.
    void check_response(const http::Response& head) {
        if (resume_from_ != 0 && head.status != kPartialContent) {
            // The server ignored Range and is sending the whole file. Restart rather than append
            // a duplicate prefix.
            kLog.info("Server ignored Range header; restarting download");
            restart();
        } else if (resume_from_ != 0) {
            check_content_range(head);
        }
        check_content_length(head);
    }

    void begin_stream() {
        out_.seek(static_cast<std::uint64_t>(resume_from_));
        position_ = resume_from_;
        written_ = resume_from_;
    }

    // One piece of the body, as one iteration of _stream's loop after its cancel check.
    void append(std::string_view chunk, const ProgressCallback& progress) {
        if (written_ + static_cast<std::int64_t>(chunk.size()) > size_) {
            throw DownloadError(text_.too_much_data);
        }
        out_.write(std::span<const char>(chunk.data(), chunk.size()));
        digest_.update(chunk);
        written_ += static_cast<std::int64_t>(chunk.size());
        position_ = written_;
        progress(kDownloading, written_, size_);
    }

    // Checks the size and digest, then flushes the file to disk.
    void verify(const ProgressCallback& progress) {
        check_canceled();
        const auto actual = position_;
        if (actual != size_) {
            throw DownloadInterrupted(replace_all(
                replace_all(text_.incomplete, "{done}", detail::format_size_bytes(actual)),
                "{total}", detail::format_size_bytes(size_)));
        }
        progress(kVerifying, actual, actual);
        check_canceled();
        if (!digest_matches()) {
            // Discarded by the caller: a retry must not resume from corrupt bytes.
            throw DownloadError(text_.integrity);
        }
        out_.sync();
        check_canceled();
    }

private:
    static std::string lowercase(std::string_view text) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), ascii_lower);
        return out;
    }

    void check_content_range(const http::Response& head) const {
        static const std::regex pattern(R"(bytes (\d+)-(\d+)/(\d+))");
        const auto value = http::find_header(head.headers, "Content-Range").value_or("");
        std::smatch match;
        if (!std::regex_match(value, match, pattern)) {
            throw DownloadError(text_.bad_resume);
        }
        const auto first = python_int(match[1].str());
        const auto last = python_int(match[2].str());
        const auto total = python_int(match[3].str());
        if (first != resume_from_ || last != size_ - 1 || total != size_) {
            throw DownloadError(text_.bad_resume);
        }
    }

    void check_content_length(const http::Response& head) const {
        const auto value = http::find_header(head.headers, "Content-Length");
        if (!value) {
            return;
        }
        const auto declared = python_int(*value);
        if (!declared) {
            throw DownloadError(text_.bad_length);
        }
        if (*declared != size_ - resume_from_) {
            throw DownloadError(text_.unexpected_length);
        }
    }

    detail::RawFile& out_;
    std::string expected_;
    std::int64_t size_;
    const CancelToken& cancel_;
    const Messages& text_;
    std::int64_t resume_from_ = 0;
    std::int64_t position_ = 0; // out.tell()
    std::int64_t written_ = 0;
    Sha256 digest_;
};

// Takes the body of one response into the partial file. Python reads the response in 1 MiB
// pieces and checks cancel before each read; the body here is gathered into pieces of that size
// so the size check and the progress reports fall where Python's do. Nothing is thrown into the
// client: the first exception is kept and the transfer stopped.
class StreamSink final : public http::BodySink {
public:
    StreamSink(PartialFile& partial, const ProgressCallback& progress, const CancelToken& cancel)
        : partial_(partial), progress_(progress), cancel_(cancel) {}

    std::optional<http::Response> refused; // a non-2xx head, not streamed
    std::exception_ptr failure;
    bool cancelled = false;

    bool on_head(const http::Response& head) override {
        if (head.status < 200 || head.status >= 300) {
            refused = head;
            return false;
        }
        return guarded([&] {
            partial_.check_response(head);
            partial_.begin_stream();
            started_ = true;
            return !stop_if_cancelled();
        });
    }

    bool write(std::span<const char> data) override {
        return guarded([&] {
            pending_.append(data.data(), data.size());
            while (pending_.size() >= kChunkBytes) {
                partial_.append(std::string_view(pending_).substr(0, kChunkBytes), progress_);
                pending_.erase(0, kChunkBytes);
            }
            return !stop_if_cancelled();
        });
    }

    // The bytes gathered but not yet written: what Python's last read would have returned.
    void flush() {
        if (!pending_.empty()) {
            const std::string chunk = std::move(pending_);
            pending_.clear();
            partial_.append(chunk, progress_);
        }
    }

    bool started() const { return started_; }

private:
    template <class F> bool guarded(F&& body) {
        try {
            return body();
        } catch (...) {
            failure = std::current_exception();
            return false;
        }
    }

    bool stop_if_cancelled() {
        if (!cancel_.cancelled()) {
            return false;
        }
        flush();
        cancelled = true;
        return true;
    }

    PartialFile& partial_;
    const ProgressCallback& progress_;
    const CancelToken& cancel_;
    std::string pending_;
    bool started_ = false;
};

struct Fetch {
    http::Client& client;
    std::string url; // the route in use; switch_route may replace it once
    const ProgressCallback& progress;
    const CancelToken& cancel;
    const DownloadOptions& options;
    const Messages& text;
    bool switched = false;
};

http::Request make_request(const Fetch& fetch, std::optional<std::int64_t> range_from) {
    http::Request request;
    request.url = fetch.url;
    request.headers.emplace_back("User-Agent", fetch.options.user_agent.empty()
                                                   ? http::user_agent()
                                                   : fetch.options.user_agent);
    if (range_from) {
        request.range_from = static_cast<std::uint64_t>(*range_from);
    }
    request.connect_timeout = kNetworkTimeout;
    request.idle_timeout = kNetworkTimeout;
    // urllib's opener follows redirects (Hugging Face sends model files through a CDN).
    request.follow_redirects = true;
    if (fetch.options.prepare) {
        fetch.options.prepare(request);
    }
    return request;
}

// The request through the caller's sender, or the client with redirects followed.
std::expected<http::Response, http::Error>
send_request(const Fetch& fetch, const http::Request& request, StreamSink& sink) {
    if (!fetch.options.send) {
        return fetch.client.send(request, fetch.cancel, &sink);
    }
    auto sent = fetch.options.send(request, fetch.cancel, sink);
    if (sent) {
        return std::move(*sent);
    }
    if (const auto* refused = std::get_if<NetworkFailure>(&sent.error())) {
        if (sink.failure) {
            std::rethrow_exception(sink.failure);
        }
        throw NetworkFailed{*refused};
    }
    return std::unexpected(std::get<http::Error>(std::move(sent.error())));
}

// One request on the current route and its body. Returns the refused head of a non-2xx answer,
// or nullopt once the body is in the file.
std::optional<http::Response> send_once(const Fetch& fetch, PartialFile& partial,
                                        std::optional<std::int64_t> range_from) {
    StreamSink sink(partial, fetch.progress, fetch.cancel);
    const auto request = make_request(fetch, range_from);
    const auto result = send_request(fetch, request, sink);
    if (sink.failure) {
        std::rethrow_exception(sink.failure);
    }
    if (sink.cancelled) {
        throw DownloadCanceled(fetch.text.canceled);
    }
    if (!result) {
        const auto& error = result.error();
        if (error.kind == http::ErrorKind::Cancelled) {
            sink.flush();
            throw DownloadCanceled(fetch.text.canceled);
        }
        if (sink.started()) {
            sink.flush();
            if (error.truncated) {
                // http.client returns a short read when the connection closes early; verify()
                // reports the shortfall and keeps the prefix.
                return std::nullopt;
            }
        }
        throw NetworkFailed{network_failure(error)};
    }
    if (sink.refused) {
        return sink.refused;
    }
    sink.flush();
    return std::nullopt;
}

// send_once, moving to the caller's other route once when it names one for a refused answer.
std::optional<http::Response> transfer(Fetch& fetch, PartialFile& partial,
                                       std::optional<std::int64_t> range_from) {
    auto refused = send_once(fetch, partial, range_from);
    if (refused && !fetch.switched && fetch.options.switch_route) {
        if (auto next = fetch.options.switch_route(refused->status)) {
            // Once per transfer: a Range or 416 restart stays on the new route.
            fetch.switched = true;
            fetch.url = std::move(*next);
            partial.check_canceled();
            refused = send_once(fetch, partial, range_from);
        }
    }
    return refused;
}

// _PartialFile.fetch_remainder: the bytes after resume_from, appended.
void fetch_remainder(Fetch& fetch, PartialFile& partial) {
    const auto resume_from = partial.resume_from();
    auto refused =
        transfer(fetch, partial, resume_from != 0 ? std::optional(resume_from) : std::nullopt);
    if (refused && refused->status == kRangeNotSatisfiable && resume_from != 0) {
        // The server cannot serve the rest; fetch the whole file instead.
        partial.restart();
        refused = transfer(fetch, partial, std::nullopt);
    }
    if (refused) {
        throw NetworkFailed{
            NetworkFailure{NetworkFailure::Kind::HttpStatus, refused->status, refused->reason}};
    }
}

void raise_if_canceled(const CancelToken& cancel, const std::filesystem::path& path,
                       bool keep_partial_on_cancel, const Messages& text) {
    if (!cancel.cancelled()) {
        return;
    }
    if (!keep_partial_on_cancel) {
        discard_regular_file(path);
    }
    throw DownloadCanceled(text.canceled);
}

void move_into_place(const std::filesystem::path& part, const std::filesystem::path& destination) {
    std::filesystem::rename(part, destination);
    std::filesystem::permissions(
        destination, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    detail::sync_directory(destination.parent_path().empty() ? std::filesystem::path(".")
                                                             : destination.parent_path());
}

// Everything that happens while the `.part` file is open. Taking the file by value closes it
// before download_verified's handlers run, as Python's `with` does.
void run(detail::RawFile out, Fetch& fetch, std::string_view sha256_hex, std::int64_t size_bytes) {
    PartialFile partial(out, sha256_hex, size_bytes, fetch.cancel, fetch.text);
    partial.load_existing();
    partial.check_canceled();
    if (partial.resume_from() == size_bytes && !partial.digest_matches()) {
        partial.restart();
    }
    if (partial.resume_from() < size_bytes) {
        fetch_remainder(fetch, partial);
    }
    partial.verify(fetch.progress);
}

} // namespace

NetworkFailure network_failure(const http::Error& error) {
    if (error.certificate) {
        return NetworkFailure{NetworkFailure::Kind::Certificate, 0, error.message};
    }
    if (error.after_head || error.after_request) {
        return NetworkFailure{NetworkFailure::Kind::Other, 0, error.message};
    }
    return NetworkFailure{NetworkFailure::Kind::Url, 0, error.message};
}

std::string describe_network_error(const NetworkFailure& failure, const NetworkErrorText& text) {
    switch (failure.kind) {
    case NetworkFailure::Kind::Certificate: {
        std::string allow;
        const auto& hosts = text.hosts;
        if (hosts.empty()) {
            allow = "the " + text.server;
        } else if (hosts.size() == 1) {
            allow = hosts.front();
        } else {
            for (std::size_t i = 0; i + 1 < hosts.size(); ++i) {
                allow += (i == 0 ? "" : ", ") + hosts[i];
            }
            allow += " and " + hosts.back();
        }
        return "The " + text.server +
               "'s certificate could not be verified. This is usually caused by network "
               "security software that inspects HTTPS traffic. Ask your IT team to allow " +
               allow + ".";
    }
    case NetworkFailure::Kind::HttpStatus: {
        if (const auto found = text.http_messages.find(failure.code);
            found != text.http_messages.end() && !found->second.empty()) {
            return found->second;
        }
        return "The " + text.server + " returned an error (" + std::to_string(failure.code) + " " +
               failure.reason + ").";
    }
    case NetworkFailure::Kind::Url:
        return "Could not reach the " + text.server + " (" + failure.reason + ").";
    case NetworkFailure::Kind::Other:
        break;
    }
    return failure.reason.empty() ? "Could not reach the " + text.server + "." : failure.reason;
}

void discard_regular_file(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error))) {
        std::filesystem::remove(path, error);
    }
}

void download_verified(http::Client& client, std::string_view url,
                       const std::filesystem::path& destination, std::int64_t size_bytes,
                       std::string_view sha256_hex, const ProgressCallback& progress,
                       const CancelToken& cancel, bool keep_partial_on_cancel,
                       const DownloadOptions& options) {
    const Messages& text = options.messages;
    if (size_bytes <= 0 || (options.max_bytes && size_bytes > *options.max_bytes)) {
        throw DownloadError(text.invalid_size);
    }
    auto part_path = destination;
    part_path += ".part";
    detail::RawFile out;
    try {
        out = detail::RawFile::open_partial(part_path);
    } catch (const std::system_error&) {
        throw DownloadError(text.unsafe_partial);
    }

    const auto describe = [&](const NetworkFailure& failure) {
        return options.describe_error ? options.describe_error(failure)
                                      : describe_network_error(failure);
    };
    Fetch fetch{client, std::string(url), progress, cancel, options, text};
    try {
        run(std::move(out), fetch, sha256_hex, size_bytes);
    } catch (const DownloadInterrupted&) {
        throw;
    } catch (const DownloadCanceled&) {
        if (!keep_partial_on_cancel) {
            discard_regular_file(part_path);
        }
        throw;
    } catch (const DownloadError&) {
        discard_regular_file(part_path);
        throw;
    } catch (const NetworkFailed& failed) {
        throw DownloadError(describe(failed.failure), failed.failure);
    } catch (const std::system_error& error) {
        const NetworkFailure failure{NetworkFailure::Kind::Other, 0, os_error_text(error)};
        throw DownloadError(describe(failure), failure);
    }

    raise_if_canceled(cancel, part_path, keep_partial_on_cancel, text);
    try {
        move_into_place(part_path, destination);
    } catch (const std::system_error&) {
        throw DownloadError(text.finalize);
    }
    raise_if_canceled(cancel, destination, keep_partial_on_cancel, text);
}

} // namespace fv::download
