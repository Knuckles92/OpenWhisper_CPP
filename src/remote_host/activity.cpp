#include "remote_host/activity.h"

#include <algorithm>
#include <chrono>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::remote_host {

Json empty_snapshot(double since) {
    Json snapshot = Json::object();
    snapshot["since"] = since;
    snapshot["transcriptions"] = 0;
    snapshot["audio_s"] = 0.0;
    snapshot["host_s"] = 0.0;
    snapshot["previews"] = 0;
    snapshot["errors"] = 0;
    snapshot["last_at"] = 0.0;
    snapshot["serial"] = 0;
    snapshot["devices"] = Json::object();
    snapshot["events"] = Json::array();
    return snapshot;
}

namespace {

std::size_t checked_max_events(int max_events) {
    if (max_events < 0) {
        throw py::ValueError("maxlen must be non-negative");
    }
    return static_cast<std::size_t>(max_events);
}

template <class Value>
Value* find(std::vector<std::pair<std::string, Value>>& items, std::string_view key) {
    for (auto& [id, value] : items) {
        if (id == key) {
            return &value;
        }
    }
    return nullptr;
}

} // namespace

HostActivity::HostActivity(Clock& clock, int max_events)
    : clock_(clock), max_events_(checked_max_events(max_events)) {
    started_at_ = now();
}

double HostActivity::now() const {
    return std::chrono::duration<double>(clock_.utc().time_since_epoch()).count();
}

void HostActivity::reset() {
    std::lock_guard lock(mutex_);
    events_.clear();
    devices_.clear();
    connections_.clear();
    started_at_ = now();
}

HostActivity::Device& HostActivity::device(std::string_view device_id, std::string_view name) {
    if (Device* found = find(devices_, device_id)) {
        if (!name.empty()) {
            found->name = std::string(name);
        }
        return *found;
    }
    devices_.emplace_back(std::string(device_id), Device{.name = std::string(name)});
    return devices_.back().second;
}

void HostActivity::event(std::string_view kind, std::string_view name, Json fields) {
    ++serial_;
    Json entry = Json::object();
    entry["serial"] = serial_;
    entry["at"] = now();
    entry["kind"] = std::string(kind);
    entry["name"] = std::string(name);
    for (auto& field : fields.items()) {
        entry[field.key()] = std::move(field.value());
    }
    if (max_events_ == 0) {
        return;
    }
    if (events_.size() == max_events_) {
        events_.pop_front();
    }
    events_.push_back(std::move(entry));
}

void HostActivity::transcribed(std::string_view device_id, std::string_view name, double audio_s,
                               double host_s) {
    std::lock_guard lock(mutex_);
    Device& entry = device(device_id, name);
    entry.transcriptions += 1;
    entry.audio_s += std::max(0.0, audio_s);
    entry.host_s += std::max(0.0, host_s);
    entry.last_at = now();
    Json fields = Json::object();
    fields["audio_s"] = py::round_ndigits(audio_s, 2);
    fields["host_s"] = py::round_ndigits(host_s, 3);
    event("transcribed", name, std::move(fields));
}

void HostActivity::previewed(std::string_view device_id, std::string_view name) {
    std::lock_guard lock(mutex_);
    Device& entry = device(device_id, name);
    entry.previews += 1;
    entry.last_at = now();
}

void HostActivity::failed(std::string_view device_id, std::string_view name,
                          std::string_view reason) {
    std::lock_guard lock(mutex_);
    Device& entry = device(device_id, name);
    entry.errors += 1;
    entry.last_at = now();
    std::string text;
    for (const auto& word : py::split(reason)) {
        if (!text.empty()) {
            text += ' ';
        }
        text += word;
    }
    if (py::len(text) > static_cast<std::size_t>(kMaxReasonChars)) {
        text = py::rstrip(py::slice(text, 0, kMaxReasonChars - 1)) + "…";
    }
    Json fields = Json::object();
    fields["reason"] = std::move(text);
    event("failed", name, std::move(fields));
}

void HostActivity::connected(std::string_view device_id, std::string_view name) {
    std::lock_guard lock(mutex_);
    int count = 1;
    if (int* open = find(connections_, device_id)) {
        count = ++*open;
    } else {
        connections_.emplace_back(std::string(device_id), 1);
    }
    device(device_id, name);
    if (count == 1) {
        event("connected", name, Json::object());
    }
}

void HostActivity::disconnected(std::string_view device_id, std::string_view name) {
    std::lock_guard lock(mutex_);
    const auto it = std::ranges::find_if(connections_,
                                         [&](const auto& item) { return item.first == device_id; });
    if (it == connections_.end() || it->second <= 0) {
        // A connection from before the last reset.
        return;
    }
    if (it->second == 1) {
        connections_.erase(it);
        event("disconnected", name, Json::object());
    } else {
        it->second -= 1;
    }
}

void HostActivity::paired(std::string_view device_id, std::string_view name, std::string_view via) {
    std::lock_guard lock(mutex_);
    device(device_id, name);
    Json fields = Json::object();
    fields["via"] = std::string(via);
    event("paired", name, std::move(fields));
}

void HostActivity::switched(std::string_view name, std::string_view label) {
    std::lock_guard lock(mutex_);
    Json fields = Json::object();
    fields["label"] = std::string(label);
    event("switched", name, std::move(fields));
}

void HostActivity::renamed(std::string_view device_id, std::string_view name) {
    std::lock_guard lock(mutex_);
    if (Device* found = find(devices_, device_id)) {
        found->name = std::string(name);
    }
}

Json HostActivity::snapshot(std::optional<int> events) const {
    Json devices = Json::object();
    Json recent = Json::array();
    double since = 0.0;
    std::int64_t serial = 0;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, entry] : devices_) {
            Json item = Json::object();
            item["name"] = entry.name;
            item["transcriptions"] = entry.transcriptions;
            item["audio_s"] = entry.audio_s;
            item["host_s"] = entry.host_s;
            item["previews"] = entry.previews;
            item["errors"] = entry.errors;
            item["last_at"] = entry.last_at;
            devices[id] = std::move(item);
        }
        for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
            recent.push_back(*it);
        }
        since = started_at_;
        serial = serial_;
    }
    if (events) {
        const auto keep = static_cast<std::size_t>(std::max(0, *events));
        if (recent.size() > keep) {
            recent.erase(recent.begin() + static_cast<std::ptrdiff_t>(keep), recent.end());
        }
    }
    Json snapshot = empty_snapshot(since);
    snapshot["serial"] = serial;
    std::int64_t transcriptions = 0;
    std::int64_t previews = 0;
    std::int64_t errors = 0;
    double audio_s = 0.0;
    double host_s = 0.0;
    double last_at = 0.0;
    for (const auto& device_item : devices.items()) {
        const Json& item = device_item.value();
        transcriptions += item.at("transcriptions").get<std::int64_t>();
        previews += item.at("previews").get<std::int64_t>();
        errors += item.at("errors").get<std::int64_t>();
        audio_s += item.at("audio_s").get<double>();
        host_s += item.at("host_s").get<double>();
        // max(a, b) keeps a unless b is greater.
        if (const double at = item.at("last_at").get<double>(); at > last_at) {
            last_at = at;
        }
    }
    snapshot["transcriptions"] = transcriptions;
    snapshot["audio_s"] = audio_s;
    snapshot["host_s"] = host_s;
    snapshot["previews"] = previews;
    snapshot["errors"] = errors;
    snapshot["last_at"] = last_at;
    snapshot["devices"] = std::move(devices);
    snapshot["events"] = std::move(recent);
    return snapshot;
}

double HostActivity::started_at() const {
    std::lock_guard lock(mutex_);
    return started_at_;
}

} // namespace fv::remote_host
