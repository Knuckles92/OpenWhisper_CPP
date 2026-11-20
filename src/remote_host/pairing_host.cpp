#include "remote_host/pairing_host.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>

#include "base/ids.h"
#include "pyport/errors.h"

namespace fv::remote_host {

namespace {

// hmac.compare_digest of two str: every byte looked at whatever differs, and TypeError for a
// string that is not ASCII.
bool compare_digest(std::string_view a, std::string_view b) {
    for (const auto text : {a, b}) {
        for (const char c : text) {
            if (static_cast<unsigned char>(c) > 0x7F) {
                throw py::TypeError("comparing strings with non-ASCII characters is not supported");
            }
        }
    }
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(i < a.size() ? a[i] : 0);
        const auto y = static_cast<unsigned char>(i < b.size() ? b[i] : 0);
        diff = static_cast<unsigned char>(diff | (x ^ y));
    }
    return diff == 0;
}

double seconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

} // namespace

std::string new_pairing_code() {
    constexpr std::uint32_t kRange = 1'000'000;
    // The largest multiple of the range below 2**32, so every code is equally likely.
    constexpr std::uint32_t kLimit = (0xFFFFFFFFu / kRange) * kRange;
    for (;;) {
        std::array<std::byte, 4> bytes{};
        random_bytes(bytes);
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data(), sizeof(value));
        if (value < kLimit) {
            return std::format("{:06}", value % kRange);
        }
    }
}

PairingHost::PairingHost(Clock& clock, std::string host_name, PairingLimits limits,
                         std::function<bool(std::string_view)> allowed_sender)
    : clock_(clock), host_name_(std::move(host_name)), limits_(limits),
      allowed_sender_(std::move(allowed_sender)) {}

void PairingHost::open(std::string code, double ttl_s) {
    const auto ttl = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(ttl_s));
    std::lock_guard lock(mutex_);
    pairing_ = Code{std::move(code), clock_.steady() + ttl, 0};
}

bool PairingHost::close() {
    std::lock_guard lock(mutex_);
    const bool had = pairing_.has_value();
    pairing_.reset();
    return had;
}

std::optional<std::pair<std::string, double>> PairingHost::status() {
    std::lock_guard lock(mutex_);
    if (!pairing_) {
        return std::nullopt;
    }
    const double left = seconds(pairing_->expires_at - clock_.steady());
    if (left <= 0) {
        pairing_.reset();
        return std::nullopt;
    }
    return std::pair{pairing_->code, left};
}

PairingHost::CodeCheck PairingHost::check_code(std::string_view code) {
    CodeCheck out;
    std::lock_guard lock(mutex_);
    if (pairing_ && pairing_->expires_at <= clock_.steady()) {
        pairing_.reset();
    }
    if (!pairing_) {
        out.refusal = PairingRefusal{"pairing_closed",
                                     "Pairing isn't open on the host. Click \"Show a pairing "
                                     "code\" there and enter the code it shows."};
    } else if (!compare_digest(code, pairing_->code)) {
        ++pairing_->failures;
        if (pairing_->failures >= MAX_PAIRING_FAILURES) {
            pairing_.reset();
            out.closed_now = true;
            out.refusal = PairingRefusal{"bad_code",
                                         "Too many wrong codes. Start pairing again on the host."};
        } else {
            out.refusal =
                PairingRefusal{"bad_code", "That code doesn't match the one on the host."};
        }
    } else {
        pairing_.reset();
    }
    return out;
}

std::variant<std::shared_ptr<PairRequest>, PairingRefusal>
PairingHost::admit_request(std::string name, std::string address) {
    if (!allowed_sender_ || !allowed_sender_(address)) {
        return PairingRefusal{"approval_refused",
                              "This host only takes pairing requests from its own network. "
                              "Enter the pairing code shown there instead."};
    }
    const auto now = clock_.steady();
    std::lock_guard lock(mutex_);
    auto& log = request_log_[address];
    while (!log.empty() && now - log.front() > limits_.approval_window) {
        log.pop_front();
    }
    if (static_cast<int>(log.size()) >= limits_.max_approval_requests) {
        return PairingRefusal{"approval_limited",
                              "Too many pairing requests from this computer. Wait a few minutes, "
                              "or enter the pairing code shown on the host."};
    }
    if (request_ && !request_->done && request_->expires_at > now) {
        return PairingRefusal{"approval_busy",
                              host_name_ +
                                  " is already asking about another computer. Try again in a "
                                  "minute."};
    }
    log.push_back(now);
    auto request = std::make_shared<PairRequest>();
    request->id = uuid4_hex().substr(0, 12);
    request->name = std::move(name);
    request->address = std::move(address);
    request->expires_at = now + limits_.handshake + limits_.approval;
    request_ = request;
    return request;
}

void PairingHost::reveal(const std::shared_ptr<PairRequest>& request, std::string sas,
                         std::function<void()> wake) {
    std::lock_guard lock(mutex_);
    request->expires_at = clock_.steady() + limits_.approval;
    request->sas = std::move(sas);
    request->wake = std::move(wake);
}

std::optional<ApprovalOutcome> PairingHost::outcome(const std::shared_ptr<PairRequest>& request,
                                                    bool socket_open) {
    std::lock_guard lock(mutex_);
    if (request->done) {
        if (request->stopped) {
            return ApprovalOutcome::stopped;
        }
        return request->decision.value_or(false) ? ApprovalOutcome::allowed
                                                 : ApprovalOutcome::denied;
    }
    if (!socket_open) {
        return ApprovalOutcome::canceled;
    }
    if (clock_.steady() >= request->expires_at) {
        return ApprovalOutcome::timeout;
    }
    return std::nullopt;
}

void PairingHost::finish(const std::shared_ptr<PairRequest>& request) {
    {
        std::lock_guard lock(mutex_);
        if (request_ == request) {
            request_.reset();
        }
        request->done = true;
        request->finished = true;
        request->wake = nullptr;
    }
    finished_.notify_all();
}

std::optional<remote::PairRequestInfo> PairingHost::pending() {
    std::lock_guard lock(mutex_);
    if (!request_ || request_->sas.empty() || request_->done) {
        return std::nullopt;
    }
    remote::PairRequestInfo info;
    info.id = request_->id;
    info.name = request_->name;
    info.address = request_->address;
    info.sas = request_->sas;
    info.seconds_left = std::max(0.0, seconds(request_->expires_at - clock_.steady()));
    return info;
}

bool PairingHost::answer(std::string_view id, bool allow) {
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        if (!request_ || request_->id != id || request_->sas.empty() || request_->done) {
            return false;
        }
        request_->decision = allow;
        request_->done = true;
        wake = request_->wake;
    }
    if (wake) {
        wake();
    }
    return true;
}

bool PairingHost::stop(std::chrono::milliseconds grace) {
    std::shared_ptr<PairRequest> request;
    std::function<void()> wake;
    bool had = false;
    {
        std::lock_guard lock(mutex_);
        had = pairing_.has_value();
        pairing_.reset();
        request = request_;
        if (request) {
            request->stopped = true;
            request->done = true;
            wake = request->wake;
        }
    }
    if (!request) {
        return had;
    }
    if (wake) {
        wake();
    }
    // The server's shutdown closes every connection, so the request says "stopped sharing"
    // first instead of just losing its line.
    std::unique_lock lock(mutex_);
    finished_.wait_for(lock, grace, [&] { return request->finished; });
    return had;
}

void PairingHost::note_request(const std::string& address) {
    std::lock_guard lock(mutex_);
    request_log_[address].push_back(clock_.steady());
}

} // namespace fv::remote_host
