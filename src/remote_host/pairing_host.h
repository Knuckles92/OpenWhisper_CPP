#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "base/clock.h"
#include "remote/control.h"

// The host's side of pairing (services/remote_asr/host.py _Pairing, PairRequest, open_pairing,
// pairing_status, pending_request, answer_request, _pair's code check and _admit_request): the
// code the host shows, and the one pairing request by approval that may wait for this
// computer's owner. SpeechHost runs the exchanges over the connections; this keeps the state
// they share with the owner's screen.
namespace fv::remote_host {

// Wrong codes allowed before the host closes pairing.
inline constexpr int MAX_PAIRING_FAILURES = 5;
inline constexpr int PAIRING_CODE_DIGITS = 6;

// secrets.randbelow(10**6), zero-padded to six digits.
std::string new_pairing_code();

// A computer on the network asking to pair, waiting for this one's owner. `sas` stays empty
// until both nonces are in; only then is the request shown, since the number is what the owner
// compares. Guarded by its PairingHost's lock.
struct PairRequest {
    std::string id; // 12 hex digits
    std::string name;
    std::string address;
    std::chrono::steady_clock::time_point expires_at;
    std::string sas;
    std::optional<bool> decision;
    bool stopped = false;  // sharing stopped while it waited
    bool done = false;     // answered, stopped, or over
    bool finished = false; // its connection sent the answer and closed
    // Asks the waiting connection to look again now (an answer or a stop arrived).
    std::function<void()> wake;
};

// Why a pairing was turned away: the error frame's code and message.
struct PairingRefusal {
    std::string code;
    std::string message;
};

// How a request waiting for the owner ended.
enum class ApprovalOutcome { allowed, denied, stopped, timeout, canceled };

struct PairingLimits {
    std::chrono::milliseconds handshake{10'000};
    std::chrono::milliseconds approval{120'000};
    std::chrono::milliseconds approval_window{600'000};
    int max_approval_requests = 4;
};

// Affinity: thread-safe. Times are `clock`'s steady time.
class PairingHost {
public:
    // `allowed_sender` decides which addresses may ask (discovery's rule: this network's).
    PairingHost(Clock& clock, std::string host_name, PairingLimits limits,
                std::function<bool(std::string_view)> allowed_sender);

    // ---- by code ----
    // Opens pairing with `code` for `ttl_s` seconds, for one successful pairing.
    void open(std::string code, double ttl_s);
    // Whether pairing was open.
    bool close();
    // (code, seconds left) while pairing is open; an expired code is forgotten here.
    std::optional<std::pair<std::string, double>> status();

    struct CodeCheck {
        std::optional<PairingRefusal> refusal; // nullopt: the code matched and is used up
        bool closed_now = false;               // the fifth wrong code closed pairing
    };
    // `code` as the client sent it, already stripped. Compared in constant time; a code with a
    // byte past ASCII is py::TypeError, as hmac.compare_digest raises for such a str.
    CodeCheck check_code(std::string_view code);

    // ---- by approval ----
    // Reserves the one request slot for `name` at `address`, or says why not:
    // approval_refused (not this network's), approval_limited (more than
    // max_approval_requests from that address within the window), approval_busy (another
    // request waits). A request expires handshake + approval from now until it is revealed.
    std::variant<std::shared_ptr<PairRequest>, PairingRefusal> admit_request(std::string name,
                                                                             std::string address);
    // Both nonces are in: the request gets its six digits and approval more time, and shows.
    // `wake` asks its connection to look again (an answer or a stop arrived); it is called
    // without the lock, from the answering or stopping thread.
    void reveal(const std::shared_ptr<PairRequest>& request, std::string sas,
                std::function<void()> wake);
    // nullopt while it should keep waiting; `socket_open` false is canceled (the client left).
    std::optional<ApprovalOutcome> outcome(const std::shared_ptr<PairRequest>& request,
                                           bool socket_open);
    // Its connection has answered: the slot is free again and stop() stops waiting for it.
    void finish(const std::shared_ptr<PairRequest>& request);

    // The request waiting for an answer here, once it shows its digits.
    std::optional<remote::PairRequestInfo> pending();
    // The owner's answer; false when that request is gone already.
    bool answer(std::string_view id, bool allow);

    // Sharing stopped: pairing closes, a waiting request is told "stopped", and this waits up to
    // `grace` for its connection to say so before the server closes every connection. Returns
    // whether pairing was open. Not on the connection's own thread.
    bool stop(std::chrono::milliseconds grace);

    // The address's recent requests, for tests that start from a busy log.
    void note_request(const std::string& address);

private:
    Clock& clock_;
    const std::string host_name_;
    const PairingLimits limits_;
    std::function<bool(std::string_view)> allowed_sender_;

    std::mutex mutex_;
    std::condition_variable finished_;
    struct Code {
        std::string code;
        std::chrono::steady_clock::time_point expires_at;
        int failures = 0;
    };
    std::optional<Code> pairing_;
    std::shared_ptr<PairRequest> request_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> request_log_;
};

} // namespace fv::remote_host
