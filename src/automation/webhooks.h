#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "automation/event_bus.h"
#include "automation/hooks_model.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "credentials/store.h"
#include "http/client.h"
#include "settings/store.h"

// Signed webhook delivery for saved webhooks, Settings > Hooks (services/automation/webhooks.py).
// Each enabled webhook receives the events it chose as the event's wire JSON (Event::to_dict),
// POSTed with Standard Webhooks headers signed by a per-webhook secret kept in the credential
// store. One bus subscription covers every enabled webhook and only enqueues; each webhook has
// its own lane, a bounded queue and a thread that delivers it in event order, so a slow or
// unreachable receiver only delays its own events. A failed attempt worth retrying waits at the
// head of its lane until its backoff passes. Event data, URLs and secrets are never logged.
namespace fv::automation {

namespace webhooks {
// Per attempt, in seconds.
inline constexpr double TIMEOUT_S = 5.0;
// Waits before the second, third and fourth attempt, in seconds.
inline constexpr double RETRY_DELAYS_S[] = {1.0, 5.0, 25.0};
// The longest a Retry-After answer may hold a webhook back, in seconds.
inline constexpr double RETRY_AFTER_MAX_S = 60.0;
// Deliveries waiting for one webhook; its oldest is dropped beyond this.
inline constexpr std::size_t MAX_QUEUED = 500;
// How long a webhook's delivery thread waits for another event before ending, in seconds.
inline constexpr double LANE_IDLE_S = 30.0;
// Recent deliveries kept per webhook (memory only).
inline constexpr std::size_t MAX_RECORDS = 50;
// Most of a reply post_signed_reply reads, in bytes.
inline constexpr std::size_t MAX_REPLY_BYTES = 1024 * 1024;
inline constexpr std::string_view TEST_EVENT_TYPE = "webhook.test";
inline constexpr std::string_view SUBSCRIPTION_LABEL = "webhooks";
// Why a delivery to a webhook that is no longer saved, with no secret, failed.
inline constexpr std::string_view SECRET_MISSING = "The signing secret is missing.";
inline constexpr std::string_view SECRET_DAMAGED =
    "The signing secret is damaged. Make a new one in Settings.";
inline constexpr std::string_view DROPPED = "Dropped: too many deliveries were waiting.";
inline constexpr std::string_view SAVE_FIRST = "Save the webhook first.";
} // namespace webhooks

// One delivery attempt, as Settings > Hooks lists it.
// Affinity: value type.
struct DeliveryRecord {
    std::string event_id;
    std::string event_type;
    std::string at; // UTC, "2026-11-16T09:30:00.123Z"
    bool ok = false;
    std::optional<int> status;
    int elapsed_ms = 0;
    std::string error;
    int attempt = 1;

    friend bool operator==(const DeliveryRecord&, const DeliveryRecord&) = default;
};

// _synthetic_event: an event for a test delivery or run, never published (seq 0), with a fresh
// "evt_" id and the clock's time.
Event synthetic_event(std::string_view type, Json data, const Clock& clock = system_clock());

// _signed_headers: Content-Type and the Standard Webhooks headers, signed at `timestamp` (Unix
// seconds; default now). Throws py::ValueError (user copy) when `secret` is not a whsec_ secret.
http::Headers signed_headers(std::string_view secret, std::string_view msg_id,
                             std::string_view body,
                             std::optional<std::int64_t> timestamp = std::nullopt);

// What a one-off signed POST (an action, an MCP Events delivery) takes beside its address.
struct SignedPost {
    // Bounds connecting, sending and (post_signed_reply) reading the reply; 0 or less is
    // webhooks::TIMEOUT_S.
    double timeout_s = webhooks::TIMEOUT_S;
    // The webhook-id; empty uses the payload's non-empty string "id", else a fresh "msg_" id.
    std::string msg_id;
    // Extra request headers (X-MCP-Subscription-Id); they never replace Content-Type or the
    // signature headers.
    http::Headers headers;
    // post_signed_reply: the most of a reply read; 0 is webhooks::MAX_REPLY_BYTES.
    std::size_t max_bytes = webhooks::MAX_REPLY_BYTES;
};

// post_signed: POSTs `payload` (an object; anything else counts as {}) as compact JSON signed
// with `secret`; one attempt, never throws. The record's event_type is payload["type"] when it is
// a string. An address that is not http(s) fails with validate_url's copy.
DeliveryRecord post_signed(http::Client& client, std::string_view url, const Json& payload,
                           std::string_view secret, const SignedPost& options = {},
                           const CancelToken& cancel = {});

// post_signed_reply: post_signed that also reads the reply. The body is the 2xx reply's bytes
// ("" when it had none, as a 204), or nullopt when the POST failed; a reply over max_bytes is
// the failure "The reply was too large". Never throws.
std::pair<DeliveryRecord, std::optional<std::string>>
post_signed_reply(http::Client& client, std::string_view url, const Json& payload,
                  std::string_view secret, const SignedPost& options = {},
                  const CancelToken& cancel = {});

// What Settings > Hooks asks of webhook delivery; WebhookEngine implements it and
// testing/fake_hook_engines.h fakes it.
// Affinity: thread-safe.
class WebhookControl {
public:
    virtual ~WebhookControl() = default;
    // Recent delivery attempts to one webhook, newest first (memory only).
    virtual std::vector<DeliveryRecord> deliveries(std::string_view hook_id) const = 0;
    // Sends a webhook.test event to one saved webhook, once, without retries, whether or not it
    // is enabled, on a thread of its own; `done` gets the record there and never an exception.
    virtual void send_test(std::string hook_id, std::function<void(DeliveryRecord)> done) = 0;
    // The webhook's signing secret, created on first use. Blocking (credential store). Throws
    // SecretUnavailable (automation/secrets.h) when it cannot be read or saved, py::ValueError
    // for an empty id.
    virtual std::string ensure_secret(std::string_view hook_id) = 0;
    // Replaces the signing secret; returns the new one. Throws as ensure_secret.
    virtual std::string rotate_secret(std::string_view hook_id) = 0;
    // Forgets a deleted webhook's secret; never throws.
    virtual void delete_secret(std::string_view hook_id) = 0;
    // Saved webhooks changed: deliver to the enabled ones from now on.
    virtual void reload() = 0;
};

// Timings and limits; tests shorten them. The clock decides when a retry is due and when an idle
// lane ends; record times come from it too.
struct WebhookOptions {
    std::vector<double> retry_delays_s{std::begin(webhooks::RETRY_DELAYS_S),
                                       std::end(webhooks::RETRY_DELAYS_S)};
    double retry_after_max_s = webhooks::RETRY_AFTER_MAX_S;
    std::size_t max_queued = webhooks::MAX_QUEUED;
    double lane_idle_s = webhooks::LANE_IDLE_S;
    const Clock* clock = nullptr; // null: system_clock()
};

// The subscription (label "webhooks") and one delivery lane per enabled webhook. stop() drops
// what waits and returns within about a second; an attempt already on its way finishes and is
// recorded. start() after stop() subscribes afresh; records survive both. The destructor stops,
// cancels attempts still on their way and waits for every thread it started.
// Affinity: thread-safe; the bus callback only enqueues.
class WebhookEngine final : public WebhookControl {
public:
    WebhookEngine(SettingsStore& settings, CredentialStore& credentials,
                  std::shared_ptr<http::Client> http, EventBus& bus, WebhookOptions options = {});
    ~WebhookEngine() override;
    WebhookEngine(const WebhookEngine&) = delete;
    WebhookEngine& operator=(const WebhookEngine&) = delete;

    // Begins delivering events to enabled webhooks; idempotent.
    void start();
    // Stops delivering; queued deliveries are dropped. Safe at quit.
    void stop();
    void reload() override;

    std::vector<DeliveryRecord> deliveries(std::string_view hook_id) const override;
    void send_test(std::string hook_id, std::function<void(DeliveryRecord)> done) override;
    std::string ensure_secret(std::string_view hook_id) override;
    std::string rotate_secret(std::string_view hook_id) override;
    void delete_secret(std::string_view hook_id) override;

    // One attempt to deliver `event` to `hook` (webhooks._deliver), not recorded: its record and
    // the wait in seconds before the next attempt (nullopt: none). The secret is made on a saved
    // webhook's first delivery, and never again once the webhook is no longer saved. Blocking.
    std::pair<DeliveryRecord, std::optional<double>> deliver(const hooks::Webhook& hook,
                                                             const Event& event, int attempt);
    // Deliveries waiting in one webhook's lane, a retry waiting for its backoff included.
    std::size_t queued(std::string_view hook_id) const;
    // Lanes whose delivery thread is running.
    std::size_t lane_threads() const;

    // Implementation (webhooks.cpp): what outlives a stop, and one start's subscription and lanes.
    struct Shared;
    struct Core;

private:
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<Core> core_; // null while stopped
    mutable std::mutex engine_mutex_;
};

} // namespace fv::automation
