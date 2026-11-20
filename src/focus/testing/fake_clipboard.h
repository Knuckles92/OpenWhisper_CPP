#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "focus/clipboard_port.h"

namespace fv::focus {

// An in-memory clipboard that records what it is asked to do. The "user's clipboard" is a text;
// staging keeps it (unless blank) under a lease, and a restore puts it back while the staged
// text is still there. Tests script selection captures with selection_answers and make writes
// fail with fail_writes.
// Affinity: thread-safe (one lock); callbacks run on the calling thread.
class FakeClipboard final : public ClipboardPort {
public:
    struct Stage {
        std::string text;
        std::optional<std::string> html;
        bool restore = true;
    };

    explicit FakeClipboard(std::string user_text = {}) : text_(std::move(user_text)) {}

    StageResult stage_text(std::string text, std::optional<std::string> html,
                           bool restore = true) override {
        std::lock_guard lock(mutex_);
        stages.push_back({text, html, restore});
        if (fail_writes) {
            return {};
        }
        const std::string user = text_;
        text_ = text;
        html_ = html.value_or("");
        if (!restore || user.find_first_not_of(" \t\r\n") == std::string::npos) {
            pending_.reset();
            return StageResult{true, std::nullopt, false};
        }
        ClipboardLease lease{++next_id_, html.value_or("")};
        pending_ = Pending{lease, user, text};
        return StageResult{true, lease, false};
    }

    bool schedule_restore(const ClipboardLease& lease, std::chrono::milliseconds delay) override {
        std::lock_guard lock(mutex_);
        restore_delays.push_back(delay);
        if (!owns(lease)) {
            pending_.reset();
            return false;
        }
        restore_scheduled_ = true;
        return true;
    }

    bool commit_text(const ClipboardLease& lease, std::string text) override {
        std::lock_guard lock(mutex_);
        if (!owns(lease)) {
            pending_.reset();
            return false;
        }
        text_ = std::move(text);
        html_ = lease.html;
        pending_.reset();
        return true;
    }

    RestoreOutcome restore_now(const ClipboardLease& lease) override {
        std::lock_guard lock(mutex_);
        return restore_locked(lease);
    }

    void cleanup() override {
        std::lock_guard lock(mutex_);
        ++cleanups;
        if (pending_) {
            restore_locked(pending_->lease);
        }
    }

    void request_prefetch(std::chrono::milliseconds delay = CLIPBOARD_PREFETCH_DELAY) override {
        std::lock_guard lock(mutex_);
        prefetches.push_back(delay);
    }

    void discard_prefetch() override {
        std::lock_guard lock(mutex_);
        ++discarded_prefetches;
    }

    void capture_selection(std::function<void()> send_copy,
                           std::function<void(std::optional<std::string>)> callback,
                           std::chrono::milliseconds timeout) override {
        std::optional<std::string> answer;
        {
            std::lock_guard lock(mutex_);
            selection_timeouts.push_back(timeout);
            if (!selection_answers.empty()) {
                answer = selection_answers.front();
                selection_answers.pop_front();
            } else {
                answer = std::string();
            }
        }
        if (answer && send_copy) {
            send_copy();
        }
        callback(std::move(answer));
    }

    bool write_text(std::string text, std::string html = {}) override {
        std::lock_guard lock(mutex_);
        writes.emplace_back(text, html);
        if (fail_writes) {
            return false;
        }
        text_ = std::move(text);
        html_ = std::move(html);
        pending_.reset();
        return true;
    }

    bool write_files(ClipboardFiles files) override {
        std::lock_guard lock(mutex_);
        if (fail_writes) {
            return false;
        }
        text_ = files.text;
        file_writes.push_back(std::move(files));
        pending_.reset();
        return true;
    }

    // Another app copies text over the clipboard.
    void external_copy(std::string text) {
        std::lock_guard lock(mutex_);
        text_ = std::move(text);
        html_.clear();
    }
    // Runs the scheduled restore as its timer would.
    RestoreOutcome fire_restore() {
        std::lock_guard lock(mutex_);
        if (!restore_scheduled_ || !pending_) {
            return RestoreOutcome::Skipped;
        }
        return restore_locked(pending_->lease);
    }
    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }
    std::string html() const {
        std::lock_guard lock(mutex_);
        return html_;
    }
    bool restore_pending() const {
        std::lock_guard lock(mutex_);
        return pending_.has_value();
    }

    // Set before use, read after; guarded by the same lock while the fake runs.
    bool fail_writes = false;
    std::deque<std::optional<std::string>> selection_answers;
    std::vector<Stage> stages;
    std::vector<std::chrono::milliseconds> restore_delays;
    std::vector<std::chrono::milliseconds> prefetches;
    std::vector<std::chrono::milliseconds> selection_timeouts;
    std::vector<std::pair<std::string, std::string>> writes;
    std::vector<ClipboardFiles> file_writes;
    int discarded_prefetches = 0;
    int cleanups = 0;

private:
    struct Pending {
        ClipboardLease lease;
        std::string user_text;
        std::string staged_text;
    };

    bool owns(const ClipboardLease& lease) const {
        return pending_ && pending_->lease == lease && text_ == pending_->staged_text;
    }

    RestoreOutcome restore_locked(const ClipboardLease& lease) {
        if (!owns(lease)) {
            if (pending_ && pending_->lease == lease) {
                pending_.reset();
            }
            return RestoreOutcome::Skipped;
        }
        text_ = pending_->user_text;
        html_.clear();
        pending_.reset();
        restore_scheduled_ = false;
        return RestoreOutcome::Restored;
    }

    mutable std::mutex mutex_;
    std::string text_;
    std::string html_;
    std::optional<Pending> pending_;
    bool restore_scheduled_ = false;
    std::uint64_t next_id_ = 0;
};

} // namespace fv::focus
