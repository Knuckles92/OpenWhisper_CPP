#include "log_test_support.h"

#include <cerrno>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "base/ids.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fv::logtest {

TempDir::TempDir()
    : path_(std::filesystem::temp_directory_path() / ("fv-log-test-" + random_hex(12))) {
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    const std::string text = read_file(path);
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

#if defined(_WIN32)

std::optional<std::uint32_t> run_child(const std::filesystem::path& program,
                                       const std::vector<std::string>& args,
                                       std::chrono::milliseconds timeout) {
    auto quote = [](const std::wstring& arg) {
        std::wstring out = L"\"";
        for (const wchar_t c : arg) {
            if (c == L'"') {
                out += L'\\';
            }
            out += c;
        }
        return out + L"\"";
    };
    std::wstring command = quote(program.native());
    for (const auto& arg : args) {
        command +=
            L" " + quote(std::filesystem::path(std::u8string(arg.begin(), arg.end())).native());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &info)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateProcessW");
    }
    CloseHandle(info.hThread);
    std::optional<std::uint32_t> result;
    if (WaitForSingleObject(info.hProcess, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        result = code;
    } else {
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 10'000);
    }
    CloseHandle(info.hProcess);
    return result;
}

#else

std::optional<std::uint32_t> run_child(const std::filesystem::path& program,
                                       const std::vector<std::string>& args,
                                       std::chrono::milliseconds timeout) {
    std::vector<std::string> owned{program.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::system_error(errno, std::generic_category(), "fork");
    }
    if (pid == 0) {
        execv(argv[0], argv.data());
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (WIFEXITED(status)) {
                return static_cast<std::uint32_t>(WEXITSTATUS(status));
            }
            return static_cast<std::uint32_t>(128 + WTERMSIG(status));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

#endif

Json untag(const Json& value) {
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(untag(item));
        }
        return out;
    }
    if (value.is_object()) {
        if (value.size() == 1 && value.contains("$float")) {
            const auto tag = value["$float"].get<std::string>();
            if (tag == "nan") {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return tag == "inf" ? std::numeric_limits<double>::infinity()
                                : -std::numeric_limits<double>::infinity();
        }
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = untag(item);
        }
        return out;
    }
    return value;
}

void CaptureSink::write(const log::Record& record) {
    std::lock_guard lock(mutex_);
    entries_.push_back({record.level, std::string(record.logger), std::string(record.message)});
}

std::vector<CaptureSink::Entry> CaptureSink::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

std::string CaptureSink::text() const {
    std::string text;
    for (const auto& entry : entries()) {
        text += entry.message + "\n";
    }
    return text;
}

CapturedLog::CapturedLog() : sink_(std::make_shared<CaptureSink>()), previous_(log::level()) {
    log::set_level(log::Level::Info);
    log::add_sink(sink_);
}

CapturedLog::~CapturedLog() {
    log::remove_sink(sink_);
    log::set_level(previous_);
}

} // namespace fv::logtest
