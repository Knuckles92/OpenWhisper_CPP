#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "focus/focus_capture.h"

namespace fv::focus {

// A threading.Event: set, clear, wait with a timeout.
// Affinity: thread-safe.
class FakeEvent {
public:
    void set() {
        {
            std::lock_guard lock(mutex_);
            set_ = true;
        }
        changed_.notify_all();
    }
    void clear() {
        std::lock_guard lock(mutex_);
        set_ = false;
    }
    bool is_set() const {
        std::lock_guard lock(mutex_);
        return set_;
    }
    // True when set within the timeout.
    bool wait(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return set_; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool set_ = false;
};

// A desktop for the capture service: the focused app is `current`, reads answer from
// `contexts` (default `context`) and block while the app's gate is closed. Mirrors the
// FakePlatform and FakeReader of OpenWhisper's tests/test_personalize_s1_service.py.
// Affinity: thread-safe.
class FakeFocusPlatform final : public FocusPlatform,
                                public std::enable_shared_from_this<FakeFocusPlatform> {
public:
    struct Read {
        std::string app_id;
        bool include_text = false;
        bool include_selection = false;
        std::thread::id thread;

        bool same_call(const std::string& app, bool text, bool selection) const {
            return app_id == app && include_text == text && include_selection == selection;
        }
    };

    struct ReaderRecord {
        std::thread::id thread;
        FakeEvent closed;
    };

    explicit FakeFocusPlatform(std::optional<AppIdentity> current, bool sync = true,
                               bool text = true)
        : current_(std::move(current)), sync_(sync), text_(text) {}

    std::string_view name() const override { return "fake"; }
    bool sync_identity() const override { return sync_; }
    bool text_supported() const override { return text_; }

    std::optional<AppIdentity> identity() override {
        std::chrono::milliseconds delay;
        {
            std::lock_guard lock(mutex_);
            delay = identity_delay;
            identity_threads.push_back(std::this_thread::get_id());
        }
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        std::lock_guard lock(mutex_);
        if (identity_error) {
            throw std::runtime_error("no desktop");
        }
        return current_;
    }

    std::unique_ptr<TextReader> text_reader() override;

    void set_current(std::optional<AppIdentity> identity) {
        std::lock_guard lock(mutex_);
        current_ = std::move(identity);
    }
    // The gate for app_id, made (closed) when missing.
    std::shared_ptr<FakeEvent> gate(const std::string& app_id) {
        std::lock_guard lock(mutex_);
        auto& gate = gates_[app_id];
        if (!gate) {
            gate = std::make_shared<FakeEvent>();
        }
        return gate;
    }
    void remove_gate(const std::string& app_id) {
        std::lock_guard lock(mutex_);
        gates_.erase(app_id);
    }
    void open_all_gates() {
        std::map<std::string, std::shared_ptr<FakeEvent>> gates;
        {
            std::lock_guard lock(mutex_);
            gates = gates_;
        }
        for (auto& [app, gate] : gates) {
            gate->set();
        }
    }
    void set_context(const std::string& app_id, TextContext context) {
        std::lock_guard lock(mutex_);
        contexts_[app_id] = std::move(context);
    }
    std::vector<Read> reads() const {
        std::lock_guard lock(mutex_);
        return reads_;
    }
    std::vector<std::shared_ptr<ReaderRecord>> readers() const {
        std::lock_guard lock(mutex_);
        return readers_;
    }
    std::vector<std::thread::id> identity_threads_seen() const {
        std::lock_guard lock(mutex_);
        return identity_threads;
    }
    void set_identity_delay(std::chrono::milliseconds delay) {
        std::lock_guard lock(mutex_);
        identity_delay = delay;
    }
    void set_identity_error(bool on) {
        std::lock_guard lock(mutex_);
        identity_error = on;
    }
    void set_reader_error(bool on) {
        std::lock_guard lock(mutex_);
        reader_error_ = on;
    }
    void set_read_error(bool on) {
        std::lock_guard lock(mutex_);
        read_error_ = on;
    }

    // What a read answers when contexts has nothing for the app.
    static TextContext default_context() {
        return TextContext{
            .before = "Dear Sam,", .caret_known = true, .selection_known = true, .source = "fake"};
    }

    // Set when a read starts waiting on a gate.
    FakeEvent blocked;
    // How long a read waits on a closed gate before going on.
    std::chrono::milliseconds gate_timeout{5000};

private:
    friend class FakeTextReader;

    std::optional<TextContext> read(const AppIdentity& identity, bool include_text,
                                    bool include_selection) {
        std::shared_ptr<FakeEvent> gate;
        {
            std::lock_guard lock(mutex_);
            reads_.push_back(
                {identity.app_id, include_text, include_selection, std::this_thread::get_id()});
            if (const auto found = gates_.find(identity.app_id); found != gates_.end()) {
                gate = found->second;
            }
        }
        if (gate) {
            blocked.set();
            gate->wait(gate_timeout);
        }
        std::lock_guard lock(mutex_);
        if (read_error_) {
            throw std::runtime_error("provider died");
        }
        const auto found = contexts_.find(identity.app_id);
        return found != contexts_.end() ? found->second : default_context();
    }

    mutable std::mutex mutex_;
    std::optional<AppIdentity> current_;
    bool sync_;
    bool text_;
    std::chrono::milliseconds identity_delay{0};
    bool identity_error = false;
    bool reader_error_ = false;
    bool read_error_ = false;
    std::vector<std::thread::id> identity_threads;
    std::map<std::string, std::shared_ptr<FakeEvent>> gates_;
    std::map<std::string, TextContext> contexts_;
    std::vector<Read> reads_;
    std::vector<std::shared_ptr<ReaderRecord>> readers_;
};

// The reader FakeFocusPlatform hands out; it records its thread and its closing.
class FakeTextReader final : public TextReader {
public:
    FakeTextReader(std::shared_ptr<FakeFocusPlatform> platform,
                   std::shared_ptr<FakeFocusPlatform::ReaderRecord> record)
        : platform_(std::move(platform)), record_(std::move(record)) {}
    ~FakeTextReader() override { record_->closed.set(); }

    std::optional<TextContext> read(const AppIdentity& identity, bool include_text,
                                    bool include_selection) override {
        return platform_->read(identity, include_text, include_selection);
    }

private:
    std::shared_ptr<FakeFocusPlatform> platform_;
    std::shared_ptr<FakeFocusPlatform::ReaderRecord> record_;
};

inline std::unique_ptr<TextReader> FakeFocusPlatform::text_reader() {
    auto record = std::make_shared<ReaderRecord>();
    record->thread = std::this_thread::get_id();
    {
        std::lock_guard lock(mutex_);
        if (reader_error_) {
            throw std::runtime_error("no UI Automation");
        }
        readers_.push_back(record);
    }
    // The reader keeps the platform alive: a thread abandoned in a read may outlive the test's
    // own reference.
    return std::make_unique<FakeTextReader>(shared_from_this(), record);
}

// A capture service that answers every request with one snapshot (OpenWhisper's tests'
// FakeService): current_identity() is `current`, defaulting to the snapshot's identity, and
// reread answers `reread_answer`.
// Affinity: thread-safe.
class FakeCaptureService final : public CaptureService {
public:
    explicit FakeCaptureService(FocusSnapshot snapshot,
                                std::optional<std::optional<AppIdentity>> current = std::nullopt)
        : snapshot_(std::move(snapshot)), current_(current ? *current : snapshot_.identity) {}

    std::shared_ptr<FocusRequest> request(bool include_text, bool include_selection) override {
        std::lock_guard lock(mutex_);
        requests.emplace_back(include_text, include_selection);
        return FocusRequest::resolved(snapshot_);
    }
    std::optional<AppIdentity> current_identity() override {
        std::lock_guard lock(mutex_);
        return current_;
    }
    void reread(const std::optional<AppIdentity>&,
                std::function<void(std::optional<TextContext>)> callback) override {
        std::optional<TextContext> answer;
        {
            std::lock_guard lock(mutex_);
            answer = reread_answer;
        }
        callback(answer);
    }
    void shutdown() override {
        std::lock_guard lock(mutex_);
        shut_down = true;
    }
    void set_current(std::optional<AppIdentity> identity) {
        std::lock_guard lock(mutex_);
        current_ = std::move(identity);
    }

    std::vector<std::pair<bool, bool>> requests;
    std::optional<TextContext> reread_answer;
    bool shut_down = false;

private:
    std::mutex mutex_;
    FocusSnapshot snapshot_;
    std::optional<AppIdentity> current_;
};

} // namespace fv::focus
