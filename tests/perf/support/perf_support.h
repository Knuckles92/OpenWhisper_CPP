#pragma once

// What every measuring test shares (M10.3): timing, percentiles, the process's resident memory,
// the machine and commit a number was measured on, the ten-times sanity ceiling read from
// tools/perf/budgets.json, and the writer of out/perf/<metric>.json. Header-only: fv_test
// compiles only the files at the top of tests/perf.
//
// A measuring test records numbers; it never fails on a budget. tools/perf/check_budgets.py
// compares them. The only assertion a test makes on a number is that it stays under ten times
// its budget, so a hang or a pathological regression still fails the suite. In a Debug build
// the ceiling is ten times wider again: budgets are for optimised builds, and an unoptimised
// build is several times slower where the work is JSON or SQL (the 739 kB meeting-state writes
// take about 65 ms in a GCC Debug build against OpenWhisper's 6.4 ms), so ten times the budget
// would fail on build type rather than on a hang. The record names its build either way.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/json.h"
#include "testkit/testkit.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// K32GetProcessMemoryInfo lives in kernel32 from Windows 7 on: no psapi.lib to link.
#include <psapi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef FV_PERF_CONFIG
#define FV_PERF_CONFIG "unknown"
#endif

namespace fvperf {

using Clock = std::chrono::steady_clock;
using fv::Json;

inline double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Linear interpolation between closest ranks, as tools/perf/perf_stats.py computes it (and
// numpy's default), so a number reads the same from either side.
inline double percentile(std::vector<double> values, double pct) {
    if (values.empty()) {
        return std::nan("");
    }
    std::ranges::sort(values);
    const double rank = (pct / 100.0) * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(std::floor(rank));
    const auto high = static_cast<std::size_t>(std::ceil(rank));
    const double fraction = rank - static_cast<double>(low);
    return values[low] + (values[high] - values[low]) * fraction;
}

inline double median(const std::vector<double>& values) {
    return percentile(values, 50);
}

// The process's resident set (Windows: the working set), MB of 1,000,000 bytes, as
// OpenWhisper's peak_rss_mb counts them.
inline std::optional<double> currentRssMb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters) == 0) {
        return std::nullopt;
    }
    return static_cast<double>(counters.WorkingSetSize) / 1'000'000.0;
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            std::istringstream fields(line.substr(6));
            double kilobytes = 0;
            fields >> kilobytes;
            return kilobytes * 1024.0 / 1'000'000.0;
        }
    }
    return std::nullopt;
#endif
}

inline std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

// What a number was measured on. No host name or user name: the report may be committed.
inline Json machine() {
    Json made = Json::object();
    made["logical_cores"] = std::thread::hardware_concurrency();
#if defined(_WIN32)
    made["platform"] = "windows";
    made["os"] = "Windows";
    if (const auto cpu = fv::testkit::get_env("PROCESSOR_IDENTIFIER")) {
        made["cpu"] = *cpu;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory) != 0) {
        made["memory_mb"] = static_cast<double>(memory.ullTotalPhys) / 1'000'000.0;
    }
#else
#if defined(__APPLE__)
    made["platform"] = "macos";
#else
    made["platform"] = "linux";
#endif
    utsname names{};
    if (uname(&names) == 0) {
        made["os"] = std::string(names.sysname) + " " + names.release;
        made["arch"] = std::string(names.machine);
    }
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.starts_with("model name")) {
            made["cpu"] = trim(line.substr(line.find(':') + 1));
            break;
        }
    }
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        made["memory_mb"] = static_cast<double>(pages) * static_cast<double>(pageSize) / 1e6;
    }
#endif
    if (const auto label = fv::testkit::get_env("OPENWHISPER_CPP_PERF_MACHINE")) {
        made["label"] = *label;
    }
    return made;
}

inline std::string readFirstLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return trim(line);
}

// OPENWHISPER_CPP_PERF_COMMIT (run_perf.py sets it), else the checkout's HEAD read from .git.
inline std::string commit() {
    if (const auto given = fv::testkit::get_env("OPENWHISPER_CPP_PERF_COMMIT");
        given && !given->empty()) {
        return *given;
    }
    const std::filesystem::path git = fv::testkit::source_root() / ".git";
    std::error_code error;
    if (!std::filesystem::is_directory(git, error)) {
        return "unknown";
    }
    std::string head = readFirstLine(git / "HEAD");
    if (!head.starts_with("ref: ")) {
        return head.empty() ? "unknown" : head;
    }
    const std::string ref = head.substr(5);
    if (std::string loose = readFirstLine(git / ref); !loose.empty()) {
        return loose;
    }
    std::ifstream packed(git / "packed-refs");
    std::string line;
    while (std::getline(packed, line)) {
        if (line.size() > 41 && line.substr(41) == ref) {
            return line.substr(0, 40);
        }
    }
    return "unknown";
}

inline std::filesystem::path outDir() {
    if (const auto given = fv::testkit::get_env("OPENWHISPER_CPP_PERF_OUT");
        given && !given->empty()) {
        return std::filesystem::path(*given);
    }
    return fv::testkit::source_root() / "out" / "perf";
}

inline std::optional<Json> readJson(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    try {
        return Json::parse(text.str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Ten times the metric's budget (budgets.json; a ratio budget times OpenWhisper's number from
// baseline-openwhisper.json), or nullopt when the metric has no absolute limit yet: a ratio
// with no reference, or an "info" metric.
inline std::optional<double> sanityCeiling(std::string_view metric) {
    const std::filesystem::path tools = fv::testkit::source_root() / "tools" / "perf";
    const auto budgets = readJson(tools / "budgets.json");
    if (!budgets || !budgets->is_array()) {
        return std::nullopt;
    }
    for (const Json& budget : *budgets) {
        if (budget.value("metric", "") != metric) {
            continue;
        }
        const std::string compare = budget.value("compare", "");
        if (!budget.contains("budget") || !budget.at("budget").is_number()) {
            return std::nullopt;
        }
        const double limit = budget.at("budget").get<double>();
        if (compare == "max") {
            return 10.0 * limit;
        }
        if (compare == "ratio") {
            const auto baseline = readJson(tools / "baseline-openwhisper.json");
            if (!baseline || !baseline->contains("metrics") ||
                !baseline->at("metrics").contains(std::string(metric))) {
                return std::nullopt;
            }
            const Json& reference = baseline->at("metrics").at(std::string(metric));
            if (!reference.contains("value") || !reference.at("value").is_number()) {
                return std::nullopt;
            }
            return 10.0 * limit * reference.at("value").get<double>();
        }
        return std::nullopt;
    }
    return std::nullopt;
}

struct Recorded {
    double p50 = 0;
    double p95 = 0;
    double max = 0;
    std::optional<double> ceiling;
};

// Writes out/perf/<metric>.json ({metric, unit, samples, p50, p95, max, commit, machine, build,
// test, notes}) and returns the statistics with the metric's sanity ceiling.
inline Recorded record(const std::string& metric, const std::string& unit,
                       const std::vector<double>& samples, const std::string& test,
                       Json notes = Json::object()) {
    Recorded out;
    out.p50 = median(samples);
    out.p95 = percentile(samples, 95);
    out.max = samples.empty() ? std::nan("") : *std::ranges::max_element(samples);
    out.ceiling = sanityCeiling(metric);

    Json made = Json::object();
    made["schema"] = 1;
    made["metric"] = metric;
    made["unit"] = unit;
    made["samples"] = samples;
    made["p50"] = out.p50;
    made["p95"] = out.p95;
    made["max"] = out.max;
    made["commit"] = commit();
    made["machine"] = machine();
    made["build"] = FV_PERF_CONFIG;
    made["test"] = test;
    made["notes"] = std::move(notes);

    const std::filesystem::path folder = outDir();
    std::filesystem::create_directories(folder);
    const std::filesystem::path target = folder / (metric + ".json");
    const std::filesystem::path partial = folder / (metric + ".json.partial");
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << made.dump(2) << "\n";
    }
    std::filesystem::rename(partial, target);
    std::printf("perf %-36s p50 %10.3f  p95 %10.3f  max %10.3f %s (n=%zu)\n", metric.c_str(),
                out.p50, out.p95, out.max, unit.c_str(), samples.size());
    return out;
}

inline constexpr bool kOptimisedBuild = std::string_view(FV_PERF_CONFIG) != "Debug";

// True when `value` is under the ceiling (ten times wider again in a Debug build), or the metric
// has none.
inline bool underCeiling(const Recorded& recorded, double value) {
    if (!recorded.ceiling) {
        return true;
    }
    return value <= *recorded.ceiling * (kOptimisedBuild ? 1.0 : 10.0);
}

} // namespace fvperf
