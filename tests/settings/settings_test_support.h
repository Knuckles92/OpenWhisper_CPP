#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "base/executor.h"
#include "base/log.h"
#include "golden/golden.h"
#include "settings/setting.h"

namespace fv::settings_test {

// A fresh folder, removed with everything in it at the end of the test.
class TempDir {
public:
    TempDir() {
        std::random_device seed;
        std::mt19937_64 random(seed());
        path_ = std::filesystem::temp_directory_path() /
                ("fv-settings-test-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// `text` as Python's text mode writes it: "\n" becomes os.linesep ("\r\n" on Windows).
inline std::string with_os_linesep(std::string_view text) {
#if defined(_WIN32)
    std::string out;
    for (const char ch : text) {
        if (ch == '\n') {
            out.push_back('\r');
        }
        out.push_back(ch);
    }
    return out;
#else
    return std::string(text);
#endif
}

inline std::vector<std::string> names_in(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

// Runs every task at once on the posting thread and is always current: Python's synchronous
// watcher callbacks.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

// The records logged while it lives.
class LogCapture {
public:
    struct Line {
        log::Level level;
        std::string logger;
        std::string message;
    };

    LogCapture() : sink_(std::make_shared<Sink>()) { log::add_sink(sink_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    ~LogCapture() { log::remove_sink(sink_); }

    std::vector<Line> lines() const {
        std::lock_guard lock(sink_->mutex);
        return sink_->lines;
    }
    bool contains(log::Level level, std::string_view text) const {
        for (const Line& line : lines()) {
            if (line.level == level && line.message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    struct Sink final : log::Sink {
        void write(const log::Record& record) override {
            std::lock_guard lock(mutex);
            lines.push_back(
                {record.level, std::string(record.logger), std::string(record.message)});
        }
        mutable std::mutex mutex;
        std::vector<Line> lines;
    };
    std::shared_ptr<Sink> sink_;
};

// A golden value as the JSON it stands for: {"$float": "nan" | "inf" | "-inf"} becomes the
// float (golden_format.py).
inline Json untag(const Json& value) {
    if (value.is_object()) {
        if (value.size() == 1 && value.contains("$float")) {
            const auto& tag = value.at("$float").get_ref<const std::string&>();
            if (tag == "nan") {
                return Json(std::nan(""));
            }
            return Json(tag == "inf" ? INFINITY : -INFINITY);
        }
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = untag(item);
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(untag(item));
        }
        return out;
    }
    return value;
}

// Equal as Python's == sees JSON data and of the same types throughout (an int is not a float,
// a bool not an int), with NaN equal to NaN: what a golden "result" must match exactly.
inline bool same(const Json& expected, const Json& actual) {
    if (expected.is_boolean() || actual.is_boolean()) {
        return expected.is_boolean() && actual.is_boolean() &&
               expected.get<bool>() == actual.get<bool>();
    }
    if (expected.is_number_float() || actual.is_number_float()) {
        if (!expected.is_number_float() || !actual.is_number_float()) {
            return false;
        }
        const double a = expected.get<double>();
        const double b = actual.get<double>();
        return (std::isnan(a) && std::isnan(b)) || a == b;
    }
    if (expected.is_number_integer() || actual.is_number_integer()) {
        if (!expected.is_number_integer() || !actual.is_number_integer()) {
            return false;
        }
        if (expected.is_number_unsigned() || actual.is_number_unsigned()) {
            return expected.dump() == actual.dump();
        }
        return expected.get<std::int64_t>() == actual.get<std::int64_t>();
    }
    if (expected.type() != actual.type()) {
        return false;
    }
    if (expected.is_array()) {
        if (expected.size() != actual.size()) {
            return false;
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (!same(expected[i], actual[i])) {
                return false;
            }
        }
        return true;
    }
    if (expected.is_object()) {
        if (expected.size() != actual.size()) {
            return false;
        }
        auto a = expected.begin();
        auto b = actual.begin();
        for (; a != expected.end(); ++a, ++b) {
            if (a.key() != b.key() || !same(a.value(), b.value())) {
                return false;
            }
        }
        return true;
    }
    return expected == actual;
}

// golden::for_each without a Catch2 section per case: sections re-run the test case once per
// section, which is quadratic over the 11,000 cases of services.setting_types. Each case runs
// once, with its label in the failure message.
inline void for_each_case(std::string_view module, std::string_view function,
                          const std::function<void(const golden::Case&)>& body) {
    const Json& document = golden::load(module);
    const auto functions = document.find("functions");
    REQUIRE(functions != document.end());
    const auto cases = functions->find(std::string(function));
    REQUIRE(cases != functions->end());
    REQUIRE(cases->is_array());
    REQUIRE_FALSE(cases->empty());
    const double tolerance = document.value("float_tolerance", 0.0);
    for (std::size_t i = 0; i < cases->size(); ++i) {
        const golden::Case one(std::string(function), i, (*cases)[i], tolerance);
        INFO(one.label());
        body(one);
    }
}

} // namespace fv::settings_test