#include "agentapi/mcp_events_registry.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "agentapi/detail/pydantic.h"
#include "automation/catalog.h"
#include "base/atomic_file.h"
#include "base/log.h"
#include "credentials/store.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::agentapi::events_ext {

namespace {

constexpr log::Logger kLog{"services.agent_mcp.events_ext"};

// Windows refuses to replace a file another handle has open (a reader, an antivirus scan or the
// search indexer); such holds last milliseconds.
constexpr int kReplaceAttempts = 20;
constexpr std::chrono::milliseconds kReplaceWait{25};
#if defined(_WIN32)
constexpr bool kRetryReplace = true;
#else
constexpr bool kRetryReplace = false;
#endif

void write_atomically(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(std::filesystem::absolute(path).parent_path());
    for (int attempt = 0;; ++attempt) {
        try {
            write_file_atomic(path, bytes);
            return;
        } catch (const std::filesystem::filesystem_error& error) {
            if (!kRetryReplace || attempt == kReplaceAttempts - 1 ||
                error.code() != std::errc::permission_denied) {
                throw;
            }
        }
        std::this_thread::sleep_for(kReplaceWait);
    }
}

bool is_public_type(const Json& name) {
    if (!name.is_string()) {
        return false;
    }
    const auto& types = automation::catalog::PUBLIC_TYPES();
    return std::ranges::find(types, name.get_ref<const std::string&>()) != types.end();
}

} // namespace

Json EventSubscription::to_record() const {
    Json out = Json::object();
    out["id"] = id;
    out["name"] = name;
    out["arguments"] = arguments;
    out["url"] = url;
    out["refresh_before"] = iso(static_cast<double>(refresh_before));
    out["created_at"] = created_at;
    out["origin"] = origin;
    return out;
}

std::optional<EventSubscription> EventSubscription::from_record(const Json& record) {
    if (!record.is_object()) {
        return std::nullopt;
    }
    const auto value = [&](const char* key) {
        const auto found = record.find(key);
        return found == record.end() ? Json(nullptr) : *found;
    };
    const Json id = value("id");
    const Json name = value("name");
    const Json url = value("url");
    const auto refresh_before = parse_iso(value("refresh_before"));
    if (!id.is_string() || !id.get_ref<const std::string&>().starts_with("sub_") ||
        !is_public_type(name) || !url.is_string() ||
        detail::has_surrogate(url.get_ref<const std::string&>()) || !refresh_before) {
        return std::nullopt;
    }
    EventSubscription out;
    try {
        out.arguments = normalize_arguments(name.get_ref<const std::string&>(), value("arguments"));
    } catch (const EventsError&) {
        return std::nullopt;
    }
    out.id = id.get<std::string>();
    out.name = name.get<std::string>();
    out.url = url.get<std::string>();
    out.refresh_before = *refresh_before;
    const Json created_at = value("created_at");
    out.created_at = created_at.is_string() ? created_at.get<std::string>() : std::string();
    const Json origin = value("origin");
    // Unknown: the more restricted of the two.
    out.origin = origin.is_string() && origin.get_ref<const std::string&>() == ORIGIN_LOCAL
                     ? std::string(ORIGIN_LOCAL)
                     : std::string(ORIGIN_TAILSCALE);
    return out;
}

Registry::Registry(std::filesystem::path path, CredentialStore& credentials, const Clock& clock,
                   std::size_t max_subscriptions)
    : path_(std::move(path)), credentials_(credentials), clock_(clock),
      max_subscriptions_(max_subscriptions), writer_(write_atomically),
      view_(std::make_shared<const Subscriptions>()) {
    load();
}

Registry::~Registry() {
    std::lock_guard lock(thread_mutex_);
    if (prune_thread_.joinable()) {
        prune_thread_.join();
    }
}

void Registry::set_writer(Writer writer) {
    std::lock_guard io(io_mutex_);
    writer_ = std::move(writer);
}

double Registry::now() const {
    return std::chrono::duration<double>(clock_.utc().time_since_epoch()).count();
}

std::shared_ptr<const Registry::Subscriptions> Registry::view() const {
    return view_.load();
}

void Registry::load() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        std::error_code ignored;
        if (std::filesystem::exists(path_, ignored)) {
            kLog.warning("Could not read MCP Events subscriptions: {}", "OSError");
        }
        return;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        kLog.warning("Could not read MCP Events subscriptions: {}", "OSError");
        return;
    }
    const std::string bytes = text.str();
    if (!decodes_as_utf8(bytes)) {
        kLog.warning("Could not read MCP Events subscriptions: {}", "UnicodeDecodeError");
        return;
    }
    Json payload;
    try {
        payload = py::loads(bytes);
    } catch (const py::JsonError&) {
        kLog.warning("Could not read MCP Events subscriptions: {}", "JSONDecodeError");
        return;
    } catch (const py::PyError& error) {
        kLog.warning("Could not read MCP Events subscriptions: {}", error.type_name());
        return;
    }
    std::lock_guard lock(mutex_);
    if (payload.is_object()) {
        const auto records = payload.find("subscriptions");
        if (records != payload.end() && records->is_array()) {
            for (const Json& record : *records) {
                if (auto subscription = EventSubscription::from_record(record)) {
                    if (EventSubscription* existing = find(subscription->id)) {
                        *existing = std::move(*subscription);
                    } else {
                        subs_.push_back(std::move(*subscription));
                    }
                }
            }
        }
    }
    commit();
}

EventSubscription* Registry::find(std::string_view id) {
    const auto found = std::ranges::find(subs_, id, &EventSubscription::id);
    return found == subs_.end() ? nullptr : &*found;
}

void Registry::commit() {
    view_.store(std::make_shared<const Subscriptions>(subs_));
}

void Registry::drop(std::string_view id) {
    std::erase_if(subs_, [&](const EventSubscription& s) { return s.id == id; });
    if (const auto found = status_.find(id); found != status_.end()) {
        status_.erase(found);
    }
    if (const auto found = previous_.find(id); found != previous_.end()) {
        previous_.erase(found);
    }
}

std::string Registry::payload() const {
    Json records = Json::array();
    for (const EventSubscription& subscription : subs_) {
        records.push_back(subscription.to_record());
    }
    Json document = Json::object();
    document["version"] = 1;
    document["subscriptions"] = std::move(records);
    py::DumpOptions options;
    options.indent = 2;
    options.ensure_ascii = false;
    return py::dumps(document, options) + "\n";
}

void Registry::write(const std::string& bytes) {
    writer_(path_, bytes);
}

void Registry::delete_secret(std::string_view id) {
    try {
        credentials_.remove(secret_name(id));
    } catch (const CredentialStoreError&) {
        kLog.warning("Could not remove the secret of MCP Events subscription {}", id);
    }
}

void Registry::restore_secret(std::string_view id, const std::optional<std::string>& value) {
    // Puts back the secret a failed save replaced; none when there was none.
    if (!value) {
        delete_secret(id);
        return;
    }
    try {
        credentials_.set(secret_name(id), *value);
    } catch (const CredentialStoreError&) {
        kLog.warning("Could not restore the secret of MCP Events subscription {}", id);
    }
}

std::optional<EventSubscription> Registry::get(std::string_view id) const {
    const auto subscriptions = view();
    const auto found = std::ranges::find(*subscriptions, id, &EventSubscription::id);
    if (found == subscriptions->end() || found->expired(now())) {
        return std::nullopt;
    }
    return *found;
}

std::vector<EventSubscription> Registry::active() const {
    const auto subscriptions = view();
    const double at = now();
    std::vector<EventSubscription> out;
    for (const EventSubscription& subscription : *subscriptions) {
        if (!subscription.expired(at)) {
            out.push_back(subscription);
        }
    }
    return out;
}

std::vector<EventSubscription> Registry::matching(std::string_view event_type,
                                                  const Json& data) const {
    std::vector<EventSubscription> out;
    for (EventSubscription& subscription : active()) {
        if (subscription.name == event_type && matches(subscription.arguments, data)) {
            out.push_back(std::move(subscription));
        }
    }
    return out;
}

bool Registry::upsert(const EventSubscription& subscription, const std::string& secret) {
    std::lock_guard io(io_mutex_);
    std::optional<EventSubscription> existing;
    {
        std::lock_guard lock(mutex_);
        if (const EventSubscription* found = find(subscription.id)) {
            existing = *found;
        }
        if (!existing) {
            const double at = now();
            const auto live = std::ranges::count_if(
                subs_, [&](const EventSubscription& s) { return !s.expired(at); });
            if (static_cast<std::size_t>(live) >= max_subscriptions_) {
                throw too_many(max_subscriptions_);
            }
        }
    }
    const bool created = !existing;
    const std::string name = secret_name(subscription.id);
    const std::optional<std::string> current = credentials_.get(name);
    if (current != secret) {
        credentials_.set(name, secret);
    }
    std::optional<std::pair<std::string, std::chrono::steady_clock::time_point>> rotation;
    std::string bytes;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = previous_.find(subscription.id); found != previous_.end()) {
            rotation = found->second;
        }
        if (current && !current->empty() && *current != secret && !created) {
            previous_[subscription.id] = {
                *current, clock_.steady() + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::duration<double>(ROTATION_WINDOW_S))};
        }
        if (EventSubscription* found = find(subscription.id)) {
            *found = subscription;
        } else {
            subs_.push_back(subscription);
        }
        commit();
        bytes = payload();
    }
    try {
        write(bytes);
    } catch (const std::system_error&) {
        {
            std::lock_guard lock(mutex_);
            if (created) {
                drop(subscription.id);
            } else {
                if (EventSubscription* found = find(subscription.id)) {
                    *found = *existing;
                }
                if (!rotation) {
                    if (const auto found = previous_.find(subscription.id);
                        found != previous_.end()) {
                        previous_.erase(found);
                    }
                } else {
                    previous_[subscription.id] = *rotation;
                }
            }
            commit();
        }
        if (created) {
            delete_secret(subscription.id);
        } else if (current != secret) {
            restore_secret(subscription.id, current);
        }
        throw;
    }
    return created;
}

bool Registry::remove(std::string_view id) {
    std::lock_guard io(io_mutex_);
    std::string bytes;
    {
        std::lock_guard lock(mutex_);
        if (find(id) == nullptr) {
            return false;
        }
        drop(id);
        commit();
        bytes = payload();
    }
    delete_secret(id);
    write(bytes);
    return true;
}

std::size_t Registry::clear() {
    std::lock_guard io(io_mutex_);
    std::size_t count = 0;
    std::vector<std::string> removed;
    std::string bytes;
    {
        std::lock_guard lock(mutex_);
        const double at = now();
        count = static_cast<std::size_t>(std::ranges::count_if(
            subs_, [&](const EventSubscription& s) { return !s.expired(at); }));
        for (const EventSubscription& subscription : subs_) {
            removed.push_back(subscription.id);
        }
        for (const std::string& id : removed) {
            drop(id);
        }
        commit();
        bytes = payload();
    }
    for (const std::string& id : removed) {
        delete_secret(id);
    }
    write(bytes);
    return count;
}

void Registry::prune() {
    std::lock_guard io(io_mutex_);
    const double at = now();
    std::vector<std::string> expired;
    std::string bytes;
    {
        std::lock_guard lock(mutex_);
        for (const EventSubscription& subscription : subs_) {
            if (subscription.expired(at)) {
                expired.push_back(subscription.id);
            }
        }
        if (expired.empty()) {
            return;
        }
        for (const std::string& id : expired) {
            drop(id);
        }
        commit();
        bytes = payload();
    }
    for (const std::string& id : expired) {
        delete_secret(id);
    }
    try {
        write(bytes);
    } catch (const std::system_error& error) {
        // They are expired either way; the next prune writes again.
        kLog.warning("Could not save MCP Events subscriptions: {}", error_name(error));
    }
}

void Registry::prune_later() {
    const double at = now();
    const auto subscriptions = view();
    if (std::ranges::none_of(*subscriptions,
                             [&](const EventSubscription& s) { return s.expired(at); })) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (pruning_) {
            return;
        }
        pruning_ = true;
    }
    std::lock_guard threads(thread_mutex_);
    if (prune_thread_.joinable()) {
        // It cleared pruning_ as its last step, so it has finished or is about to.
        prune_thread_.join();
    }
    prune_thread_ = std::thread([this] {
        try {
            prune();
        } catch (const std::exception& error) {
            kLog.warning("Could not forget expired MCP Events subscriptions: {}",
                         error_name(error));
        }
        std::lock_guard lock(mutex_);
        pruning_ = false;
    });
}

std::vector<std::string> Registry::signing_secrets(std::string_view id) {
    const auto current = credentials_.get(secret_name(id));
    if (!current || current->empty()) {
        return {};
    }
    std::vector<std::string> keys{*current};
    std::lock_guard lock(mutex_);
    if (const auto found = previous_.find(id); found != previous_.end()) {
        const auto steady = clock_.steady();
        if (found->second.second > steady && found->second.first != *current) {
            keys.push_back(found->second.first);
        } else if (found->second.second <= steady) {
            previous_.erase(found);
        }
    }
    return keys;
}

void Registry::record(std::string_view id, std::string_view error) {
    std::lock_guard lock(mutex_);
    if (find(id) == nullptr) {
        return;
    }
    auto found = status_.find(id);
    if (found == status_.end()) {
        found = status_.emplace(std::string(id), Status{}).first;
    }
    Status& status = found->second;
    const std::string at = iso(now());
    if (!error.empty()) {
        status.last_error = std::string(error);
        if (!status.failed_since) {
            status.failed_since = at;
        }
    } else {
        status.last_delivery_at = at;
        status.last_error.reset();
        status.failed_since.reset();
    }
}

Json Registry::delivery_status(std::string_view id, bool active) const {
    std::lock_guard lock(mutex_);
    Json out = Json::object();
    out["active"] = active;
    const auto found = status_.find(id);
    if (found == status_.end()) {
        return out;
    }
    const Status& status = found->second;
    if (status.last_delivery_at) {
        out["lastDeliveryAt"] = *status.last_delivery_at;
    }
    if (status.last_error) {
        out["lastError"] = *status.last_error;
    }
    if (status.failed_since) {
        out["failedSince"] = *status.failed_since;
    }
    return out;
}

} // namespace fv::agentapi::events_ext
