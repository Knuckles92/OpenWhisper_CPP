#pragma once

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "remote/record_sync_control.h"
#include "remote/wire.h"

namespace fv::remote::testing {

// A RecordSyncControl with scripted answers: every call is recorded ("record_saved(dictation,r1)",
// "bring_back()", ...), set_location changes `status_value.location`, list_remote returns
// `listing[kind]` and remembers each item for remote_item, and `fail_next` makes the next call
// that can fail throw instead. paused_for_backup counts pauses held (`pauses`).
// Affinity: thread-safe once scripted; a test writes the public fields only while no other
// thread is calling in.
class FakeRecordSync : public RecordSyncControl {
public:
    SyncStatus status_value;
    int send_existing_result = 0;
    int bring_back_result = 0;
    std::vector<std::string> progress_lines; // handed to bring_back's progress, in order
    std::map<std::string, std::vector<Json>> listing;
    std::filesystem::path audio_path;
    std::set<std::string> on_host; // record ids copies_on_host answers with
    bool wanted = false;
    bool deleted_result = true;

    void fail_next(std::exception_ptr error) {
        std::lock_guard lock(mutex_);
        failure_ = std::move(error);
    }
    std::vector<std::string> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    int pauses() const {
        std::lock_guard lock(mutex_);
        return pauses_;
    }

    void record_saved(std::string_view kind, std::string_view id) override {
        record(std::format("record_saved({},{})", kind, id));
    }
    void record_edited(std::string_view kind, std::string_view id) override {
        record(std::format("record_edited({},{})", kind, id));
    }
    void record_deleted(std::string_view kind, std::string_view id) override {
        record(std::format("record_deleted({},{})", kind, id));
    }
    void cleared(std::string_view kind) override { record(std::format("cleared({})", kind)); }
    void cleared(std::string_view kind, std::span<const std::string> keep) override {
        std::string kept;
        for (const auto& id : keep) {
            kept += kept.empty() ? id : "," + id;
        }
        record(std::format("cleared({},[{}])", kind, kept));
    }

    std::string location() override {
        std::lock_guard lock(mutex_);
        return status_value.location;
    }
    void set_location(std::string_view location) override {
        record(std::format("set_location({})", location));
        if (std::find(RECORD_LOCATIONS.begin(), RECORD_LOCATIONS.end(), location) ==
            RECORD_LOCATIONS.end()) {
            throw std::invalid_argument(std::format("Unknown storage location: '{}'", location));
        }
        {
            std::lock_guard lock(mutex_);
            status_value.location = std::string(location);
        }
        changed.emit("");
    }
    SyncStatus status() override {
        std::lock_guard lock(mutex_);
        return status_value;
    }
    void refresh_summary() override {
        record("refresh_summary()");
        throw_if_failing();
    }
    int send_existing() override {
        record("send_existing()");
        throw_if_failing();
        std::lock_guard lock(mutex_);
        return send_existing_result;
    }
    int bring_back(std::function<void(std::string)> progress) override {
        record("bring_back()");
        throw_if_failing();
        std::vector<std::string> lines;
        int result = 0;
        {
            std::lock_guard lock(mutex_);
            lines = progress_lines;
            result = bring_back_result;
        }
        if (progress) {
            for (const auto& line : lines) {
                progress(line);
            }
        }
        changed.emit("records");
        return result;
    }
    void wake(bool now) override { record(std::format("wake({})", now ? "true" : "false")); }
    std::vector<Json> list_remote(std::string_view kind, std::string_view query,
                                  int limit) override {
        record(std::format("list_remote({},{},{})", kind, query, limit));
        throw_if_failing();
        std::lock_guard lock(mutex_);
        const auto found = listing.find(std::string(kind));
        if (found == listing.end()) {
            return {};
        }
        for (const auto& item : found->second) {
            if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                remembered_[{std::string(kind), item["id"].get<std::string>()}] = item;
            }
        }
        return found->second;
    }
    std::optional<Json> remote_item(std::string_view kind, std::string_view record_id) override {
        std::lock_guard lock(mutex_);
        const auto found = remembered_.find({std::string(kind), std::string(record_id)});
        return found == remembered_.end() ? std::nullopt : std::optional(found->second);
    }
    bool delete_remote(std::string_view kind, std::string_view record_id) override {
        record(std::format("delete_remote({},{})", kind, record_id));
        throw_if_failing();
        bool result = false;
        {
            std::lock_guard lock(mutex_);
            remembered_.erase({std::string(kind), std::string(record_id)});
            result = deleted_result;
        }
        changed.emit("records");
        return result;
    }
    std::filesystem::path audio_for(std::string_view record_id) override {
        record(std::format("audio_for({})", record_id));
        throw_if_failing();
        std::lock_guard lock(mutex_);
        return audio_path;
    }
    void check_out(std::string_view kind, std::string_view record_id,
                   std::function<void(std::int64_t, std::int64_t)> progress) override {
        record(std::format("check_out({},{})", kind, record_id));
        throw_if_failing();
        if (progress) {
            progress(1, 1);
        }
        changed.emit("records");
    }
    std::set<std::string> copies_on_host(std::string_view kind,
                                         std::span<const std::string> ids) override {
        record(std::format("copies_on_host({})", kind));
        std::lock_guard lock(mutex_);
        std::set<std::string> found;
        for (const auto& id : ids) {
            if (on_host.contains(id)) {
                found.insert(id);
            }
        }
        return found;
    }
    bool listing_wanted(std::string_view kind) override {
        record(std::format("listing_wanted({})", kind));
        std::lock_guard lock(mutex_);
        return wanted;
    }
    SyncPause paused_for_backup() override {
        record("paused_for_backup()");
        throw_if_failing();
        {
            std::lock_guard lock(mutex_);
            ++pauses_;
        }
        return SyncPause([this] {
            std::lock_guard lock(mutex_);
            --pauses_;
        });
    }

private:
    void record(std::string call) {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::move(call));
    }
    void throw_if_failing() {
        std::exception_ptr failure;
        {
            std::lock_guard lock(mutex_);
            failure = std::exchange(failure_, nullptr);
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
    std::exception_ptr failure_;
    std::map<std::pair<std::string, std::string>, Json> remembered_;
    int pauses_ = 0;
};

} // namespace fv::remote::testing
