#pragma once

// OpenWhisper's duck-typed `Repo` of the live-signal and citation tests: get_segment and
// get_segments over a list of rows the test edits while a worker runs. Every other repository
// method fails the test (StubRepository).

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../meeting_state/stub_repository.h"
#include "base/json.h"

namespace fv::meeting_intel::test {

class RowsRepo final : public meeting_state::test_support::StubRepository {
public:
    explicit RowsRepo(std::vector<Json> rows = {}) : rows_(std::move(rows)) {}

    // Python's Repo.get_segments: rows starting after `after_start_s`, in list order.
    std::vector<Json> get_segments(std::string_view, double after_start_s,
                                   std::optional<std::int64_t> limit) override {
        std::lock_guard lock(mutex_);
        std::vector<Json> out;
        for (const auto& row : rows_) {
            if (row.value("start_s", 0.0) > after_start_s &&
                (!limit || static_cast<std::int64_t>(out.size()) < *limit)) {
                out.push_back(row);
            }
        }
        return out;
    }
    std::optional<Json> get_segment(std::string_view, std::string_view id) override {
        std::lock_guard lock(mutex_);
        for (const auto& row : rows_) {
            if (row.value("id", "") == id) {
                return std::optional<Json>(std::in_place, row);
            }
        }
        return std::nullopt;
    }

    void add(Json row) {
        std::lock_guard lock(mutex_);
        rows_.push_back(std::move(row));
    }
    void set_text(std::size_t index, std::string text) {
        std::lock_guard lock(mutex_);
        rows_.at(index)["text"] = std::move(text);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        rows_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<Json> rows_;
};

// Waits until `done` holds (a worker went idle), polling; false after 3 s.
inline bool eventually(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

} // namespace fv::meeting_intel::test
