#pragma once

// What the meeting_intel tests share: a temporary folder, file writers, the fixture folder, a
// settings provider and a stub HistorySource like the stub repositories of OpenWhisper's tests.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "base/json.h"
#include "base/log.h"
#include "base/utf8.h"
#include "meeting_intel/history_source.h"
#include "testkit/testkit.h"

namespace fv::meeting_intel::test {

namespace fs = std::filesystem;

// The messages of the "meeting" loggers, DEBUG included, written while it lives.
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<Sink>()) {
        log::set_logger_level("meeting", log::Level::Debug);
        log::add_sink(sink_);
    }
    ~LogCapture() {
        log::remove_sink(sink_);
        log::set_logger_level("meeting", std::nullopt);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::string text() const {
        std::lock_guard lock(sink_->mutex);
        return sink_->text;
    }
    // How many messages contain `part`.
    std::size_t count(std::string_view part) const {
        std::lock_guard lock(sink_->mutex);
        std::size_t found = 0;
        for (const auto& line : sink_->lines) {
            found += line.find(part) != std::string::npos ? 1 : 0;
        }
        return found;
    }

private:
    struct Sink final : log::Sink {
        void write(const log::Record& record) override {
            if (!record.logger.starts_with("meeting")) {
                return;
            }
            std::lock_guard lock(mutex);
            lines.emplace_back(record.message);
            text.append(record.message);
            text += '\n';
        }
        mutable std::mutex mutex;
        std::vector<std::string> lines;
        std::string text;
    };
    std::shared_ptr<Sink> sink_;
};

// A new empty folder under the system temporary folder, removed with everything in it.
class TempFolder {
public:
    TempFolder() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("fv-meeting-intel-" + std::to_string(stamp) + "-" + std::to_string(++counter));
        fs::create_directories(path_);
        path_ = fs::canonical(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline fs::path write_bytes(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

// Path.write_text(text, encoding="utf-8"); relative parts use "/".
inline fs::path write_text(const fs::path& root, std::string_view relpath, std::string_view text) {
    return write_bytes(root / base_utf8::path_from_utf8(relpath), text);
}

inline fs::path knowledge_fixtures() {
    return testkit::source_root() / "tests" / "fixtures" / "meeting_intel" / "knowledge";
}

inline std::string utf8(const fs::path& path) {
    return base_utf8::path_to_utf8(path);
}

// The settings a patched resolve_meeting_context_folder_* returns.
inline std::function<Json()> folder_settings(bool enabled, const std::string& path) {
    return [enabled, path] {
        return Json{{"meeting_context_folder_enabled", enabled},
                    {"meeting_context_folder_path", path}};
    };
}

inline std::function<Json()> fixed(Json settings) {
    return [settings = std::move(settings)] {
        return settings;
    };
}

// A HistorySource answering from fixed rows, recording what it was asked.
class StubHistory final : public HistorySource {
public:
    struct SearchCall {
        std::string query;
        std::optional<std::string> exclude_meeting_id;
        std::int64_t limit = 0;
    };
    struct CandidateCall {
        std::string query;
        std::optional<std::string> exclude_meeting_id;
        std::int64_t limit = 0;
        bool shared_only = false;
    };

    std::vector<Json> rows;
    std::vector<Json> candidates;
    std::map<std::string, Json, std::less<>> meetings;
    std::map<std::string, std::vector<Json>, std::less<>> segments;
    // Set to make search_candidates throw, as a repository without it would.
    bool candidates_fail = false;

    std::optional<SearchCall> search_call;
    std::vector<CandidateCall> candidate_calls;
    std::vector<std::string> meeting_reads;

    std::vector<Json> search_transcripts(std::string_view query,
                                         const std::optional<std::string>& exclude_meeting_id,
                                         std::int64_t limit) override {
        search_call = SearchCall{std::string(query), exclude_meeting_id, limit};
        return rows;
    }
    std::vector<Json> search_candidates(std::string_view query,
                                        const std::optional<std::string>& exclude_meeting_id,
                                        std::int64_t limit, bool shared_only) override {
        candidate_calls.push_back({std::string(query), exclude_meeting_id, limit, shared_only});
        if (candidates_fail) {
            throw std::logic_error("no candidate search");
        }
        return candidates;
    }
    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        meeting_reads.emplace_back(meeting_id);
        const auto found = meetings.find(meeting_id);
        if (found == meetings.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    std::vector<Json> get_segments(std::string_view meeting_id, std::int64_t limit) override {
        const auto found = segments.find(meeting_id);
        if (found == segments.end()) {
            return {};
        }
        std::vector<Json> rows_out = found->second;
        if (limit > 0 && rows_out.size() > static_cast<std::size_t>(limit)) {
            rows_out.resize(static_cast<std::size_t>(limit));
        }
        return rows_out;
    }
};

} // namespace fv::meeting_intel::test
