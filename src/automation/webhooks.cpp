#include "automation/webhooks.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <typeinfo>
#include <utility>

#include "automation/detail/pyvalue.h"
#include "automation/detail/worker_threads.h"
#include "automation/hook_http.h"
#include "automation/hooks_model.h"
#include "automation/secrets.h"
#include "automation/signing.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::automation {

namespace {

constexpr log::Logger kLog{"services.automation.webhooks"};

using SteadyTime = std::chrono::steady_clock::time_point;

std::chrono::steady_clock::duration seconds(double value) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(value));
}

// The Python class name a failure reads as in copy ("The delivery failed (ValueError).").
std::string type_name(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return "ValueError";
    }
    if (dynamic_cast<const CredentialStoreError*>(&error) != nullptr) {
        return "CredentialStoreError";
    }
    return "RuntimeError";
}

// _secret_name.
std::string secret_name(std::string_view hook_id) {
    const std::string id = py::strip(hook_id);
    if (id.empty()) {
        throw py::ValueError("This webhook has no id.");
    }
    return hooks::webhook_secret_name(id);
}

// _store_copy.
std::string store_copy(const std::exception& error) {
    if (dynamic_cast<const CredentialStoreError*>(&error) != nullptr) {
        return std::string("Couldn't keep the webhook's signing secret. ") + error.what();
    }
    return std::format("Couldn't keep the webhook's signing secret ({}).", type_name(error));
}

DeliveryRecord failed_record(std::string event_id, std::string event_type, std::string at,
                             std::string error, int attempt) {
    return DeliveryRecord{std::move(event_id), std::move(event_type),
                          std::move(at),       false,
                          std::nullopt,        0,
                          std::move(error),    attempt};
}

std::pair<DeliveryRecord, std::optional<std::string>>
post_once(http::Client& client, std::string_view url, const Json& given, std::string_view secret,
          const SignedPost& options, std::size_t read_limit, const CancelToken& cancel) {
    const std::string at = hook_http::iso_now();
    const Json payload = given.is_object() ? given : Json::object();
    std::string msg_id = options.msg_id;
    if (msg_id.empty()) {
        const auto id = payload.find("id");
        msg_id =
            id != payload.end() && id->is_string() && !id->get_ref<const std::string&>().empty()
                ? id->get<std::string>()
                : "msg_" + random_hex(24);
    }
    const auto type = payload.find("type");
    const std::string event_type =
        type != payload.end() && type->is_string() ? type->get<std::string>() : std::string();
    const auto failed = [&](std::string error) {
        return std::pair<DeliveryRecord, std::optional<std::string>>{
            failed_record(msg_id, event_type, at, std::move(error), 1), std::nullopt};
    };

    try {
        hooks::validate_url(url);
    } catch (const py::ValueError& error) {
        return failed(error.what());
    }
    // json.dumps(ensure_ascii=False) keeps a lone surrogate, which encode("utf-8") then refuses.
    if (!detail::encodes_as_utf8(payload)) {
        return failed("The message isn't valid JSON.");
    }
    std::string body = py::dumps(payload, {.ensure_ascii = false, .item_sep = ",", .key_sep = ":"});
    http::Headers signed_part;
    try {
        signed_part = signed_headers(secret, msg_id, body);
    } catch (const py::ValueError& error) {
        return failed(error.what());
    }
    std::set<std::string> taken;
    for (const auto& [name, value] : signed_part) {
        taken.insert(py::lower(name));
    }
    http::Headers headers;
    for (const auto& [name, value] : options.headers) {
        if (!taken.contains(py::lower(name))) {
            headers.emplace_back(name, value);
        }
    }
    headers.insert(headers.end(), signed_part.begin(), signed_part.end());
    const double timeout = options.timeout_s > 0 ? options.timeout_s : webhooks::TIMEOUT_S;
    std::optional<SteadyTime> deadline;
    if (read_limit > 0) {
        deadline = std::chrono::steady_clock::now() + seconds(timeout);
    }
    hook_http::Result outcome = hook_http::exchange(client, url, std::move(body), headers, timeout,
                                                    read_limit, deadline, cancel);
    DeliveryRecord record{
        msg_id, event_type, at, outcome.ok, outcome.status, outcome.elapsed_ms, outcome.error, 1};
    std::optional<std::string> reply;
    if (outcome.ok) {
        reply = std::move(outcome.body);
    }
    return {std::move(record), std::move(reply)};
}

} // namespace

Event synthetic_event(std::string_view type, Json data, const Clock& clock) {
    Event event;
    event.id = "evt_" + random_hex(24);
    event.seq = 0;
    event.type = std::string(type);
    event.time = hook_http::iso_now(clock);
    event.data = std::move(data);
    return event;
}

http::Headers signed_headers(std::string_view secret, std::string_view msg_id,
                             std::string_view body, std::optional<std::int64_t> timestamp) {
    http::Headers headers{{"Content-Type", std::string(hook_http::CONTENT_TYPE)}};
    for (auto& header : signing::headers(secret, msg_id, body, timestamp)) {
        headers.push_back(std::move(header));
    }
    return headers;
}

DeliveryRecord post_signed(http::Client& client, std::string_view url, const Json& payload,
                           std::string_view secret, const SignedPost& options,
                           const CancelToken& cancel) {
    try {
        return post_once(client, url, payload, secret, options, 0, cancel).first;
    } catch (const std::exception& error) {
        kLog.warning("A signed POST failed: {}", type_name(error));
        return failed_record(options.msg_id, "", hook_http::iso_now(),
                             std::format("The request failed ({}).", type_name(error)), 1);
    }
}

std::pair<DeliveryRecord, std::optional<std::string>>
post_signed_reply(http::Client& client, std::string_view url, const Json& payload,
                  std::string_view secret, const SignedPost& options, const CancelToken& cancel) {
    try {
        const std::size_t limit =
            options.max_bytes > 0 ? options.max_bytes : webhooks::MAX_REPLY_BYTES;
        return post_once(client, url, payload, secret, options, limit, cancel);
    } catch (const std::exception& error) {
        kLog.warning("A signed POST for a reply failed: {}", type_name(error));
        return {failed_record(options.msg_id, "", hook_http::iso_now(),
                              std::format("The request failed ({}).", type_name(error)), 1),
                std::nullopt};
    }
}

// -- what outlives one start/stop: records, secrets and the threads ------------------------------

struct WebhookEngine::Shared {
    Shared(SettingsStore& settings, CredentialStore& credentials,
           std::shared_ptr<http::Client> http, EventBus& bus, WebhookOptions options)
        : settings(settings), credentials(credentials), http(std::move(http)), bus(bus),
          options(std::move(options)),
          clock(this->options.clock != nullptr ? *this->options.clock : system_clock()) {}

    SettingsStore& settings;
    CredentialStore& credentials;
    std::shared_ptr<http::Client> http;
    EventBus& bus;
    WebhookOptions options;
    const Clock& clock;

    std::mutex secret_mutex;
    mutable std::mutex records_mutex;
    std::map<std::string, std::deque<DeliveryRecord>, std::less<>> records;
    // Cancelled only by the destructor: attempts on their way at stop() finish as in Python.
    CancelSource cancel;
    detail::WorkerThreads threads;

    // _remember.
    void remember(const std::string& hook_id, DeliveryRecord record) {
        std::lock_guard lock(records_mutex);
        auto& kept = records[hook_id];
        kept.push_front(std::move(record));
        while (kept.size() > webhooks::MAX_RECORDS) {
            kept.pop_back();
        }
    }

    // _forget_records.
    void forget_records(const std::set<std::string, std::less<>>& keep) {
        std::lock_guard lock(records_mutex);
        std::erase_if(records, [&](const auto& entry) { return !keep.contains(entry.first); });
    }

    // _delivery_secret: nullopt once the webhook is no longer saved, so a delivery or test on its
    // way never brings back a deleted webhook's secret (Settings removes the webhook before it
    // calls delete_secret, and this checks settings under the same lock).
    std::optional<std::string> delivery_secret(std::string_view hook_id) {
        const std::string name = secret_name(hook_id);
        std::lock_guard lock(secret_mutex);
        try {
            if (auto existing = credentials.get(name); existing && !existing->empty()) {
                return existing;
            }
            if (!hooks::find_webhook(*settings.snapshot(), hook_id)) {
                return std::nullopt;
            }
            std::string secret = signing::new_secret();
            credentials.set(name, secret);
            return secret;
        } catch (const std::exception& error) {
            throw SecretUnavailable(store_copy(error));
        }
    }

    // _deliver: one attempt; the record and the wait before a retry (nullopt: done).
    std::pair<DeliveryRecord, std::optional<double>> deliver(const hooks::Webhook& hook,
                                                             const Event& event, int attempt) {
        const std::string at = hook_http::iso_now(clock);
        std::string body;
        http::Headers headers;
        try {
            const auto secret = delivery_secret(hook.id);
            if (!secret) {
                return {failed_record(event.id, event.type, at,
                                      std::string(webhooks::SECRET_MISSING), attempt),
                        std::nullopt};
            }
            body = event.to_json();
            headers = signed_headers(*secret, event.id, body);
        } catch (const SecretUnavailable& error) {
            return {failed_record(event.id, event.type, at, error.what(), attempt), std::nullopt};
        } catch (const py::ValueError&) {
            return {failed_record(event.id, event.type, at, std::string(webhooks::SECRET_DAMAGED),
                                  attempt),
                    std::nullopt};
        }
        const hook_http::Result outcome =
            hook_http::exchange(*http, hook.url, std::move(body), headers, webhooks::TIMEOUT_S, 0,
                                std::nullopt, cancel.token());
        DeliveryRecord record{event.id,           event.type,    at,     outcome.ok, outcome.status,
                              outcome.elapsed_ms, outcome.error, attempt};
        const auto& delays = options.retry_delays_s;
        if (outcome.ok || !outcome.retryable || attempt > static_cast<int>(delays.size())) {
            return {std::move(record), std::nullopt};
        }
        double delay = delays[static_cast<std::size_t>(attempt - 1)];
        if (outcome.retry_after) {
            delay = std::max(delay, std::min(*outcome.retry_after, options.retry_after_max_s));
        }
        return {std::move(record), delay};
    }

    // _attempt: deliver, with an unexpected failure as a record and no retry.
    std::pair<DeliveryRecord, std::optional<double>> attempt(const hooks::Webhook& hook,
                                                             const Event& event, int attempt) {
        try {
            return deliver(hook, event, attempt);
        } catch (const std::exception& error) {
            kLog.warning("Webhook delivery failed: {}", type_name(error));
            return {failed_record(event.id, event.type, hook_http::iso_now(clock),
                                  std::format("The delivery failed ({}).", type_name(error)),
                                  attempt),
                    std::nullopt};
        }
    }

    // _send_test.
    DeliveryRecord send_test(const std::string& hook_id) {
        const auto hook = hooks::find_webhook(*settings.snapshot(), hook_id);
        if (!hook) {
            return failed_record("", std::string(webhooks::TEST_EVENT_TYPE),
                                 hook_http::iso_now(clock), std::string(webhooks::SAVE_FIRST), 1);
        }
        Event event = synthetic_event(
            webhooks::TEST_EVENT_TYPE,
            Json{{"message", std::format("This is a test delivery from {}.", identity().product)},
                 {"webhook_id", hook->id}},
            clock);
        DeliveryRecord record = deliver(*hook, event, 1).first;
        remember(hook->id, record);
        return record;
    }
};

// -- one start: the subscription and the lanes
// -----------------------------------------------------

struct WebhookEngine::Core : std::enable_shared_from_this<WebhookEngine::Core> {
    struct Item {
        Event event;
        int attempt = 1;
        SteadyTime not_before{};
    };
    // One webhook's waiting deliveries and whether a thread is sending them. Touched only with
    // `mutex` held.
    struct Lane {
        std::deque<Item> queue;
        std::condition_variable wake;
        bool running = false;
    };

    explicit Core(std::shared_ptr<Shared> shared) : shared(std::move(shared)) {}

    std::shared_ptr<Shared> shared;
    std::mutex configure_mutex;
    std::mutex mutex;
    std::condition_variable lane_ended;
    std::vector<hooks::Webhook> hooks; // enabled, in saved order
    std::map<std::string, std::shared_ptr<Lane>, std::less<>> lanes;
    std::uint64_t dropped = 0;
    std::shared_ptr<EventSubscription> subscription;
    std::set<std::string, std::less<>> types;
    bool subscribed = false; // Python's "bus is self._bus": the first configure always subscribes
    std::int64_t last_seq = 0;
    bool stopping = false;

    const hooks::Webhook* hook(std::string_view id) const {
        for (const auto& hook : hooks) {
            if (hook.id == id) {
                return &hook;
            }
        }
        return nullptr;
    }

    static bool wants(const hooks::Webhook& hook, std::string_view type) {
        return std::ranges::find(hook.events, type) != hook.events.end();
    }

    // Waits on `cv` until `due` on the engine's clock or a notify. A clock other than the
    // system's is looked at again every 20 ms, because it moves without notifying anyone.
    void wait_until(std::unique_lock<std::mutex>& lock, std::condition_variable& cv,
                    SteadyTime due) {
        auto remaining = due - shared->clock.steady();
        if (&shared->clock != &system_clock()) {
            remaining = std::min<std::chrono::steady_clock::duration>(
                remaining, std::chrono::milliseconds(20));
        }
        if (remaining > std::chrono::steady_clock::duration::zero()) {
            cv.wait_for(lock, remaining);
        }
    }

    void configure(std::vector<hooks::Webhook> saved) {
        std::vector<hooks::Webhook> enabled;
        std::set<std::string, std::less<>> wanted;
        for (auto& hook : saved) {
            if (hook.enabled && !hook.events.empty()) {
                wanted.insert(hook.events.begin(), hook.events.end());
                enabled.push_back(std::move(hook));
            }
        }
        std::lock_guard configuring(configure_mutex);
        std::shared_ptr<EventSubscription> old;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            hooks = std::move(enabled);
            for (auto it = lanes.begin(); it != lanes.end();) {
                const hooks::Webhook* current = hook(it->first);
                Lane& lane = *it->second;
                if (current == nullptr) {
                    // Deleted or turned off: what waits is dropped and the lane's thread ends
                    // (an attempt in flight finishes).
                    lane.queue.clear();
                    lane.wake.notify_all();
                    it = lanes.erase(it);
                    continue;
                }
                std::erase_if(lane.queue,
                              [&](const Item& item) { return !wants(*current, item.event.type); });
                ++it;
            }
            if (subscribed && wanted == types) {
                return;
            }
            old = subscription;
            subscribed = true;
            types = wanted;
        }
        // Subscribe before closing the old one so no event slips between them; one delivered to
        // both is dropped by its sequence number.
        std::shared_ptr<EventSubscription> fresh;
        if (!wanted.empty()) {
            std::weak_ptr<Core> weak = weak_from_this();
            fresh = shared->bus.subscribe(
                [weak](const Event& event) {
                    if (auto core = weak.lock()) {
                        core->on_event(event);
                    }
                },
                std::vector<std::string>(wanted.begin(), wanted.end()),
                std::string(webhooks::SUBSCRIPTION_LABEL));
        }
        {
            std::lock_guard lock(mutex);
            subscription = fresh;
        }
        if (old) {
            old->close();
        }
    }

    void stop() {
        std::vector<std::shared_ptr<Lane>> running;
        std::shared_ptr<EventSubscription> closing;
        {
            std::lock_guard configuring(configure_mutex);
            {
                std::lock_guard lock(mutex);
                stopping = true;
                for (auto& [id, lane] : lanes) {
                    lane->queue.clear();
                    lane->wake.notify_all();
                    if (lane->running) {
                        running.push_back(lane);
                    }
                }
                lanes.clear();
                closing = std::move(subscription);
            }
            if (closing) {
                closing->close();
            }
        }
        // Attempts in flight finish on their own; waiting for them is bounded.
        std::unique_lock lock(mutex);
        lane_ended.wait_for(lock, std::chrono::seconds(1), [&] {
            return std::ranges::none_of(running, [](const auto& lane) { return lane->running; });
        });
    }

    // The bus callback (dispatch thread): only enqueues.
    void on_event(const Event& event) {
        std::lock_guard lock(mutex);
        if (stopping || event.seq <= last_seq) {
            return;
        }
        last_seq = event.seq;
        for (const auto& hook : hooks) {
            if (wants(hook, event.type)) {
                enqueue(hook.id, Item{event}, false);
            }
        }
    }

    // Caller holds `mutex`.
    void enqueue(const std::string& hook_id, Item item, bool front) {
        auto& slot = lanes[hook_id];
        if (!slot) {
            slot = std::make_shared<Lane>();
        }
        const std::shared_ptr<Lane> lane = slot;
        if (front) {
            lane->queue.push_front(std::move(item));
        } else {
            lane->queue.push_back(std::move(item));
        }
        while (lane->queue.size() > shared->options.max_queued) {
            drop_oldest(hook_id, *lane);
        }
        if (!lane->running) {
            lane->running = true;
            shared->threads.spawn(
                "automation-webhooks-" + hook_id,
                [core = shared_from_this(), hook_id, lane] { core->run(hook_id, lane); });
        }
        lane->wake.notify_one();
    }

    // Caller holds `mutex`.
    void drop_oldest(const std::string& hook_id, Lane& lane) {
        Item item = std::move(lane.queue.front());
        lane.queue.pop_front();
        ++dropped;
        if (dropped == 1 || dropped % 100 == 0) {
            kLog.warning("Webhook deliveries are backing up; dropped the oldest ({} so far)",
                         dropped);
        }
        shared->remember(hook_id, failed_record(item.event.id, item.event.type,
                                                hook_http::iso_now(shared->clock),
                                                std::string(webhooks::DROPPED), item.attempt));
    }

    // _next: the lane's next delivery once it is due; nullopt when the lane should end.
    std::optional<std::pair<hooks::Webhook, Item>> next(const std::string& hook_id,
                                                        const std::shared_ptr<Lane>& lane) {
        std::unique_lock lock(mutex);
        std::optional<SteadyTime> idle_until;
        while (!stopping) {
            const auto found = lanes.find(hook_id);
            if (found == lanes.end() || found->second != lane) {
                break;
            }
            const SteadyTime now = shared->clock.steady();
            if (!lane->queue.empty()) {
                idle_until.reset();
                if (lane->queue.front().not_before <= now) {
                    Item item = std::move(lane->queue.front());
                    lane->queue.pop_front();
                    return std::pair{*hook(hook_id), std::move(item)};
                }
                wait_until(lock, lane->wake, lane->queue.front().not_before);
                continue;
            }
            if (!idle_until) {
                idle_until = now + seconds(shared->options.lane_idle_s);
            } else if (now >= *idle_until) {
                break;
            }
            wait_until(lock, lane->wake, *idle_until);
        }
        // The next event starts a new thread (enqueue).
        lane->running = false;
        lane_ended.notify_all();
        return std::nullopt;
    }

    void run(const std::string& hook_id, const std::shared_ptr<Lane>& lane) {
        while (auto picked = next(hook_id, lane)) {
            auto& [hook, item] = *picked;
            auto [record, delay] = shared->attempt(hook, item.event, item.attempt);
            shared->remember(hook.id, std::move(record));
            if (delay) {
                retry_later(hook, lane, std::move(item), *delay);
            }
        }
    }

    // Puts `item` back at the front of its lane, unless the hook or lane changed meanwhile.
    void retry_later(const hooks::Webhook& hook, const std::shared_ptr<Lane>& lane, Item item,
                     double delay) {
        std::lock_guard lock(mutex);
        const hooks::Webhook* current = this->hook(hook.id);
        const auto found = lanes.find(hook.id);
        if (stopping || found == lanes.end() || found->second != lane || current == nullptr ||
            !wants(*current, item.event.type)) {
            return;
        }
        item.attempt += 1;
        item.not_before = shared->clock.steady() + seconds(delay);
        enqueue(hook.id, std::move(item), true);
    }
};

// -- the engine
// ------------------------------------------------------------------------------------

WebhookEngine::WebhookEngine(SettingsStore& settings, CredentialStore& credentials,
                             std::shared_ptr<http::Client> http, EventBus& bus,
                             WebhookOptions options)
    : shared_(std::make_shared<Shared>(settings, credentials, std::move(http), bus,
                                       std::move(options))) {}

WebhookEngine::~WebhookEngine() {
    stop();
    shared_->cancel.cancel();
    shared_->threads.join_all();
}

void WebhookEngine::start() {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        if (!core_) {
            core_ = std::make_shared<Core>(shared_);
        }
        core = core_;
    }
    auto saved = hooks::load_webhooks(*shared_->settings.snapshot());
    std::set<std::string, std::less<>> keep;
    for (const auto& hook : saved) {
        keep.insert(hook.id);
    }
    shared_->forget_records(keep);
    core->configure(std::move(saved));
}

void WebhookEngine::stop() {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        core = std::exchange(core_, nullptr);
    }
    if (core) {
        core->stop();
    }
}

void WebhookEngine::reload() {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        core = core_;
    }
    if (!core) {
        return;
    }
    auto saved = hooks::load_webhooks(*shared_->settings.snapshot());
    std::set<std::string, std::less<>> keep;
    for (const auto& hook : saved) {
        keep.insert(hook.id);
    }
    shared_->forget_records(keep);
    core->configure(std::move(saved));
}

std::vector<DeliveryRecord> WebhookEngine::deliveries(std::string_view hook_id) const {
    std::lock_guard lock(shared_->records_mutex);
    const auto found = shared_->records.find(hook_id);
    if (found == shared_->records.end()) {
        return {};
    }
    return {found->second.begin(), found->second.end()};
}

void WebhookEngine::send_test(std::string hook_id, std::function<void(DeliveryRecord)> done) {
    shared_->threads.spawn("webhook-test", [shared = shared_, hook_id = std::move(hook_id),
                                            done = std::move(done)] {
        DeliveryRecord record;
        try {
            record = shared->send_test(hook_id);
        } catch (const std::exception& error) {
            kLog.warning("A webhook test couldn't run: {}", type_name(error));
            record = failed_record("", std::string(webhooks::TEST_EVENT_TYPE),
                                   hook_http::iso_now(shared->clock),
                                   std::format("The test couldn't run ({}).", type_name(error)), 1);
        }
        if (done) {
            try {
                done(std::move(record));
            } catch (const std::exception& error) {
                kLog.warning("A webhook test's answer failed: {}", type_name(error));
            }
        }
    });
}

std::string WebhookEngine::ensure_secret(std::string_view hook_id) {
    const std::string name = secret_name(hook_id);
    std::lock_guard lock(shared_->secret_mutex);
    try {
        if (auto existing = shared_->credentials.get(name); existing && !existing->empty()) {
            return *existing;
        }
        std::string secret = signing::new_secret();
        shared_->credentials.set(name, secret);
        return secret;
    } catch (const std::exception& error) {
        throw SecretUnavailable(store_copy(error));
    }
}

std::string WebhookEngine::rotate_secret(std::string_view hook_id) {
    const std::string name = secret_name(hook_id);
    std::string secret = signing::new_secret();
    std::lock_guard lock(shared_->secret_mutex);
    try {
        shared_->credentials.set(name, secret);
    } catch (const std::exception& error) {
        throw SecretUnavailable(store_copy(error));
    }
    return secret;
}

void WebhookEngine::delete_secret(std::string_view hook_id) {
    try {
        const std::string name = secret_name(hook_id);
        std::lock_guard lock(shared_->secret_mutex);
        shared_->credentials.remove(name);
    } catch (const std::exception& error) {
        kLog.warning("Could not remove a webhook's signing secret: {}", type_name(error));
    }
}

std::pair<DeliveryRecord, std::optional<double>>
WebhookEngine::deliver(const hooks::Webhook& hook, const Event& event, int attempt) {
    return shared_->deliver(hook, event, attempt);
}

std::size_t WebhookEngine::queued(std::string_view hook_id) const {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        core = core_;
    }
    if (!core) {
        return 0;
    }
    std::lock_guard lock(core->mutex);
    const auto found = core->lanes.find(hook_id);
    return found == core->lanes.end() ? 0 : found->second->queue.size();
}

std::size_t WebhookEngine::lane_threads() const {
    std::shared_ptr<Core> core;
    {
        std::lock_guard lock(engine_mutex_);
        core = core_;
    }
    if (!core) {
        return 0;
    }
    std::lock_guard lock(core->mutex);
    return static_cast<std::size_t>(std::ranges::count_if(
        core->lanes, [](const auto& entry) { return entry.second->running; }));
}

} // namespace fv::automation
