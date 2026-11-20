#include "log/failure_journal.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>

#include "log/detail/files.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::log {

namespace {

// diagnostics._COMPONENTS, in order.
constexpr std::array<std::string_view, 11> kComponents{
    "recorder",   "audio_processor", "application_controller",
    "database",   "history_manager", "runtime.transcription",
    "components", "local_asr",       "streaming_transcriber",
    "app_update", "remote_engine",
};

// The other components _failure_events accepts.
constexpr std::array<std::string_view, 4> kOtherComponents{"transcriber", "ui", "meeting",
                                                           "application"};

// The names of diagnostics._EXCEPTIONS, then "Error" and "LoggedError".
constexpr std::array<std::string_view, 10> kCategories{
    "TimeoutError", "PermissionError", "FileNotFoundError", "MemoryError", "ConnectionError",
    "OSError",      "ValueError",      "RuntimeError",      "Error",       "LoggedError",
};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& names, std::string_view name) {
    for (const auto candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

std::string_view os_error_category(const std::error_code& code) {
    if (code == std::errc::timed_out) {
        return "TimeoutError";
    }
    if (code == std::errc::permission_denied || code == std::errc::operation_not_permitted) {
        return "PermissionError";
    }
    if (code == std::errc::no_such_file_or_directory) {
        return "FileNotFoundError";
    }
    if (code == std::errc::connection_aborted || code == std::errc::connection_refused ||
        code == std::errc::connection_reset || code == std::errc::broken_pipe) {
        return "ConnectionError";
    }
    return "OSError";
}

} // namespace

std::string_view component_for_logger(std::string_view logger) {
    for (const auto known : kComponents) {
        const std::string exact = "services." + std::string(known);
        if (logger == exact || logger.starts_with(exact + ".")) {
            return known;
        }
    }
    std::string_view component = "application";
    for (const auto& [prefix, name] :
         {std::pair<std::string_view, std::string_view>{"transcriber.", "transcriber"},
          {"ui_qt.", "ui"},
          {"meeting.", "meeting"}}) {
        if (logger.starts_with(prefix)) {
            component = name;
        }
    }
    return component;
}

std::string_view category_for_exception(const std::exception* error) {
    if (!error) {
        return "LoggedError";
    }
    if (dynamic_cast<const std::bad_alloc*>(error)) {
        return "MemoryError";
    }
    if (const auto* os = dynamic_cast<const std::system_error*>(error)) {
        return os_error_category(os->code());
    }
    if (dynamic_cast<const py::ValueError*>(error) ||
        dynamic_cast<const std::invalid_argument*>(error) ||
        dynamic_cast<const std::domain_error*>(error) ||
        dynamic_cast<const std::out_of_range*>(error)) {
        return "ValueError";
    }
    if (const auto* python = dynamic_cast<const py::PyError*>(error)) {
        // OverflowError, IndexError and TypeError are none of _EXCEPTIONS; RecursionError is a
        // RuntimeError.
        return python->type_name() == "RecursionError" ? "RuntimeError" : "Error";
    }
    if (dynamic_cast<const std::runtime_error*>(error)) {
        return "RuntimeError";
    }
    return "Error";
}

Json failure_events(const Json& value) {
    Json result = Json::array();
    if (!value.is_array()) {
        return result;
    }
    const std::size_t start = value.size() > kMaxFailures ? value.size() - kMaxFailures : 0;
    for (std::size_t i = start; i < value.size(); ++i) {
        const Json& event = value[i];
        if (!event.is_object()) {
            continue;
        }
        const auto component = event.find("component");
        if (component == event.end() || !component->is_string() ||
            !(contains(kComponents, component->get_ref<const std::string&>()) ||
              contains(kOtherComponents, component->get_ref<const std::string&>()))) {
            continue;
        }
        const auto category = event.find("category");
        if (category == event.end() || !category->is_string() ||
            !contains(kCategories, category->get_ref<const std::string&>())) {
            continue;
        }
        const auto utc = event.find("utc");
        if (utc == event.end() || !utc->is_string()) {
            continue; // fromisoformat(None or a number) is TypeError
        }
        std::string timestamp;
        try {
            timestamp = py::fromisoformat(utc->get_ref<const std::string&>()).isoformat();
        } catch (const py::PyError&) {
            continue;
        }
        Json line = nullptr;
        if (const auto found = event.find("line"); found != event.end()) {
            // type(line) is int: a JSON integer, never a bool or a float.
            if (found->is_number_unsigned()) {
                if (const auto number = found->get<std::uint64_t>();
                    number > 0 && number < 100000) {
                    line = static_cast<std::int64_t>(number);
                }
            } else if (found->is_number_integer()) {
                if (const auto number = found->get<std::int64_t>(); number > 0 && number < 100000) {
                    line = number;
                }
            }
        }
        Json kept = Json::object();
        kept["utc"] = timestamp;
        kept["component"] = *component;
        kept["category"] = *category;
        kept["line"] = line;
        result.push_back(std::move(kept));
    }
    return result;
}

struct FailureJournalSink::State {
    std::filesystem::path path;
    mutable std::mutex mutex;
    Json events = Json::array();

    void save() {
        std::string text;
        {
            std::lock_guard lock(mutex);
            py::DumpOptions options;
            options.indent = 2;
            text = py::dumps(events, options);
        }
        try {
            detail::write_text_replace(path, text);
        } catch (const std::exception&) {
            // Disk-full and permission failures must not log here: that would recurse.
        }
    }
};

FailureJournalSink::FailureJournalSink(std::filesystem::path path, LogWriter& writer)
    : state_(std::make_shared<State>()), queue_(writer.queue()),
      job_(writer.coalesced([state = state_] { state->save(); })) {
    state_->path = std::move(path);
    state_->events = failure_events(detail::read_json_file(state_->path));
}

FailureJournalSink::~FailureJournalSink() = default;

void FailureJournalSink::write(const Record& record) {
    if (record.level < Level::Error) {
        return;
    }
    Json event = Json::object();
    event["utc"] = detail::utc_isoformat(record.time);
    event["component"] = std::string(component_for_logger(record.logger));
    event["category"] = std::string(category_for_exception(record.exception));
    event["line"] = static_cast<std::int64_t>(record.line);
    {
        std::lock_guard lock(state_->mutex);
        auto& events = state_->events;
        while (events.size() >= kMaxFailures) {
            events.erase(events.begin());
        }
        events.push_back(std::move(event));
    }
    job_.wake();
}

void FailureJournalSink::flush(std::chrono::milliseconds budget) {
    queue_.flush(budget);
}

Json FailureJournalSink::events() const {
    std::lock_guard lock(state_->mutex);
    return state_->events;
}

const std::filesystem::path& FailureJournalSink::path() const {
    return state_->path;
}

} // namespace fv::log
