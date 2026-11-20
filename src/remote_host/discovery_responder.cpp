#include "remote_host/discovery_responder.h"

#include <exception>
#include <format>
#include <utility>

#include "base/log.h"
#include "remote/discovery_codec.h"
#include "remote/wire.h"

namespace fv::remote_host {

namespace {

constexpr log::Logger logger("services.remote_asr.discovery");

constexpr auto kPoll = std::chrono::milliseconds(500); // RESPONDER_POLL_S
constexpr auto kRateWindow = std::chrono::seconds(10); // RATE_WINDOW_S

} // namespace

DiscoveryResponder::DiscoveryResponder(std::function<Json()> info, Clock& clock)
    : info_(std::move(info)), clock_(clock) {}

DiscoveryResponder::~DiscoveryResponder() {
    stop();
}

bool DiscoveryResponder::start(const remote::DiscoveryTransportFactory& transports,
                               std::optional<int> port, std::string_view bind) {
    std::lock_guard lock(state_mutex_);
    if (transport_) {
        return true;
    }
    const int wanted = port.value_or(remote::DISCOVERY_PORT);
    std::shared_ptr<remote::DiscoveryTransport> transport;
    if (transports) {
        transport = transports(false);
    }
    if (!transport) {
        error_ = std::format("UDP port {} is in use (no socket)", wanted);
        logger.warning("Remote engine discovery could not listen on UDP {}: no socket", wanted);
        return false;
    }
    const auto bound = transport->bind(bind, static_cast<std::uint16_t>(wanted));
    if (!bound) {
        transport->close();
        error_ = std::format("UDP port {} is in use ({})", wanted, bound.error());
        logger.warning("Remote engine discovery could not listen on UDP {}: {}", wanted,
                       bound.error());
        return false;
    }
    transport_ = transport;
    port_ = *bound;
    error_.clear();
    stopping_ = false;
    thread_ = std::thread([this, transport] { serve(transport); });
    logger.info("Remote engine discovery answering on UDP {}:{}", bind, *port_);
    return true;
}

void DiscoveryResponder::stop() {
    std::shared_ptr<remote::DiscoveryTransport> transport;
    std::thread thread;
    {
        std::lock_guard lock(state_mutex_);
        stopping_ = true;
        transport = std::exchange(transport_, nullptr);
        thread = std::move(thread_);
        port_.reset();
    }
    if (transport) {
        transport->close();
    }
    if (thread.joinable()) {
        thread.join();
    }
}

bool DiscoveryResponder::running() const {
    std::lock_guard lock(state_mutex_);
    return transport_ != nullptr;
}

std::optional<std::uint16_t> DiscoveryResponder::port() const {
    std::lock_guard lock(state_mutex_);
    return port_;
}

std::string DiscoveryResponder::error() const {
    std::lock_guard lock(state_mutex_);
    return error_;
}

void DiscoveryResponder::serve(std::shared_ptr<remote::DiscoveryTransport> transport) {
    while (!stopping_) {
        // A timeout, a closed socket or (Windows) an ICMP "port unreachable" left over from an
        // earlier answer: none ends the loop unless stop() asked.
        auto datagram = transport->receive(kPoll);
        if (!datagram) {
            continue;
        }
        try {
            const auto reply = answer(datagram->bytes, datagram->sender);
            if (reply && !transport->send_to(*reply, datagram->sender, datagram->port)) {
                logger.debug("Could not answer a discovery query");
            }
        } catch (const std::exception&) {
            logger.warning("Remote engine discovery answer failed");
        }
    }
}

std::optional<std::vector<std::byte>> DiscoveryResponder::answer(std::span<const std::byte> data,
                                                                 std::string_view sender) {
    const auto nonce = remote::parse_query(data);
    if (!nonce || !remote::allowed_sender(sender) || !admit(std::string(sender))) {
        return std::nullopt;
    }
    auto reply = remote::encode_reply(*nonce, info_());
    if (reply.empty()) {
        return std::nullopt;
    }
    return reply;
}

bool DiscoveryResponder::admit(const std::string& sender) {
    const auto now = clock_.steady();
    std::lock_guard lock(rate_mutex_);
    auto& mine = per_sender_[sender];
    for (auto* times : {&all_, &mine}) {
        while (!times->empty() && now - times->front() > kRateWindow) {
            times->pop_front();
        }
    }
    if (static_cast<int>(mine.size()) >= remote::MAX_ANSWERS_PER_SENDER ||
        static_cast<int>(all_.size()) >= remote::MAX_ANSWERS) {
        return false;
    }
    mine.push_back(now);
    all_.push_back(now);
    if (per_sender_.size() > remote::MAX_TRACKED_SENDERS) {
        // Forget senders with nothing recent, so the map stays small.
        std::erase_if(per_sender_, [](const auto& entry) { return entry.second.empty(); });
    }
    return true;
}

} // namespace fv::remote_host
