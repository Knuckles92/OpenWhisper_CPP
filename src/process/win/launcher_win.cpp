#include "process/launcher.h"

#include <wil/resource.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

#include "base/log.h"
#include "base/utf8.h"
#include "process/detail/start_error.h"
#include "process/detail/text.h"
#include "process/process_env.h"
#include "process/run_once.h"
#include "process/win/job.h"
#include "process/win/pipes.h"
#include "process/win/wide.h"

namespace fv {

namespace {

constexpr log::Logger logger("services.agent_process");

using process_win::widen;

class WinProcess final : public Process {
public:
    WinProcess(wil::unique_handle process, DWORD pid, wil::unique_handle job,
               wil::unique_handle stdin_write, wil::unique_handle stdout_read,
               wil::unique_handle stderr_read, bool disown_on_clean_exit)
        : process_(std::move(process)), pid_(pid), job_(std::move(job)),
          stdout_read_(std::move(stdout_read)), stderr_read_(std::move(stderr_read)),
          disown_on_clean_exit_(disown_on_clean_exit) {
        had_job_ = static_cast<bool>(job_);
        if (stdin_write) {
            writer_ = std::make_unique<process_win::PipeWriter>(stdin_write.release(),
                                                                thread_name(L"in"));
        }
    }

    ~WinProcess() override {
        release_pipes(std::chrono::milliseconds(0));
        std::lock_guard lock(mutex_);
        if (job_) {
            if (disown_on_clean_exit_ && exited()) {
                process_win::clear_job_limits(job_.get());
            }
            // Without the cleared limit, closing the last handle ends every process in the job.
            job_.reset();
        }
    }

    std::uint32_t pid() const override { return pid_; }

    void write_stdin(std::string_view data) override {
        std::lock_guard lock(mutex_);
        if (writer_) {
            writer_->write(data);
        }
    }

    void close_stdin() override {
        std::lock_guard lock(mutex_);
        if (writer_) {
            writer_->close();
        }
    }

    void on_line(LineSink stdout_sink, LineSink stderr_sink) override {
        on_bytes(stdout_sink ? process_detail::line_splitter(std::move(stdout_sink)) : nullptr,
                 stderr_sink ? process_detail::line_splitter(std::move(stderr_sink)) : nullptr);
    }

    void on_bytes(ByteSink stdout_sink, ByteSink stderr_sink) override {
        std::lock_guard lock(mutex_);
        if (readers_started_) {
            throw std::logic_error("A process's output is already being read.");
        }
        readers_started_ = true;
        if (stdout_read_) {
            stdout_reader_ = std::make_unique<process_win::PipeReader>(
                stdout_read_.release(), thread_name(L"out"), std::move(stdout_sink));
        }
        if (stderr_read_) {
            stderr_reader_ = std::make_unique<process_win::PipeReader>(
                stderr_read_.release(), thread_name(L"err"), std::move(stderr_sink));
        }
    }

    std::optional<int> wait(std::chrono::milliseconds timeout) override {
        const auto count = timeout.count();
        const DWORD ms = count <= 0                                  ? 0
                         : count >= static_cast<long long>(INFINITE) ? INFINITE - 1
                                                                     : static_cast<DWORD>(count);
        if (WaitForSingleObject(process_.get(), ms) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        return exit_code();
    }

    std::optional<int> exit_code() const override {
        if (!exited()) {
            return std::nullopt;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(process_.get(), &code)) {
            return std::nullopt;
        }
        return static_cast<int>(code);
    }

    void kill_tree() override {
        std::lock_guard lock(mutex_);
        if (job_) {
            TerminateJobObject(job_.get(), 1);
            job_.reset();
        } else if (exited()) {
            return;
        } else if (had_job_) {
            // Disowned: what it started is no longer this process's to end.
            TerminateProcess(process_.get(), 1);
        } else {
            process_win::terminate_tree(pid_);
        }
    }

    void disown() override {
        std::lock_guard lock(mutex_);
        if (!job_) {
            return;
        }
        if (!process_win::clear_job_limits(job_.get())) {
            logger.debug("Could not release a program's job (error {})", GetLastError());
        }
        job_.reset();
    }

    void release_pipes(std::chrono::milliseconds grace) override {
        std::lock_guard release(release_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + grace;
        process_win::PipeWriter* writer = nullptr;
        process_win::PipeReader* out = nullptr;
        process_win::PipeReader* err = nullptr;
        {
            std::lock_guard lock(mutex_);
            writer = writer_.get();
            out = stdout_reader_.get();
            err = stderr_reader_.get();
            stdout_read_.reset();
            stderr_read_.reset();
        }
        for (process_win::PipeThread* thread : {static_cast<process_win::PipeThread*>(writer),
                                                static_cast<process_win::PipeThread*>(out),
                                                static_cast<process_win::PipeThread*>(err)}) {
            if (thread != nullptr) {
                thread->finish(deadline);
            }
        }
    }

private:
    bool exited() const { return WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0; }

    std::wstring thread_name(const wchar_t* stream) const {
        return std::format(L"fv-pipe-{}-{}", pid_, stream);
    }

    mutable std::mutex mutex_;
    std::mutex release_mutex_;
    wil::unique_handle process_;
    DWORD pid_;
    wil::unique_handle job_;
    bool had_job_ = false;
    wil::unique_handle stdout_read_;
    wil::unique_handle stderr_read_;
    bool disown_on_clean_exit_;
    bool readers_started_ = false;
    std::unique_ptr<process_win::PipeWriter> writer_;
    std::unique_ptr<process_win::PipeReader> stdout_reader_;
    std::unique_ptr<process_win::PipeReader> stderr_reader_;
};

bool has_nul(std::string_view text) {
    return text.find('\0') != std::string_view::npos;
}

SECURITY_ATTRIBUTES inheritable() {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    return attributes;
}

wil::unique_handle open_nul(DWORD access) {
    auto attributes = inheritable();
    HANDLE handle = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                OPEN_EXISTING, 0, nullptr);
    return wil::unique_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

// This process's standard handle as the child's, as subprocess passes one it wasn't told to
// redirect; NUL when there is none.
wil::unique_handle inherited_standard(DWORD which, DWORD nul_access) {
    HANDLE own = GetStdHandle(which);
    if (own != nullptr && own != INVALID_HANDLE_VALUE) {
        HANDLE copy = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &copy, 0, TRUE,
                            DUPLICATE_SAME_ACCESS)) {
            return wil::unique_handle(copy);
        }
    }
    return open_nul(nul_access);
}

// A pipe whose `child_end` is inheritable and whose other end is not.
bool make_pipe(wil::unique_handle& read, wil::unique_handle& write, bool child_reads) {
    auto attributes = inheritable();
    HANDLE r = nullptr;
    HANDLE w = nullptr;
    if (!CreatePipe(&r, &w, &attributes, 0)) {
        return false;
    }
    read.reset(r);
    write.reset(w);
    SetHandleInformation(child_reads ? write.get() : read.get(), HANDLE_FLAG_INHERIT, 0);
    return true;
}

// "name=value\0...\0\0" in UTF-16, sorted by upper-cased name as Windows expects.
std::wstring environment_block(const std::map<std::string, std::string>& env) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    for (const auto& [name, value] : env) {
        entries.emplace_back(widen(name), widen(value));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.first.c_str(), static_cast<int>(a.first.size()),
                                    b.first.c_str(), static_cast<int>(b.first.size()),
                                    TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& [name, value] : entries) {
        block += name;
        block += L'=';
        block += value;
        block += L'\0';
    }
    if (block.empty()) {
        block += L'\0';
    }
    block += L'\0';
    return block;
}

class WinLauncher final : public ProcessLauncher {
public:
    std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec& spec) override {
        const std::string program = path_to_utf8(spec.program);
        const std::string cwd = path_to_utf8(spec.cwd);
        if (program.empty() || has_nul(program) || has_nul(cwd) ||
            std::any_of(spec.args.begin(), spec.args.end(), has_nul)) {
            return std::unexpected(std::string(ARGUMENTS_NOT_VALID));
        }
        const auto env = spec.env.empty() ? to_spec_env(child_env()) : spec.env;
        for (const auto& [name, value] : env) {
            if (name.empty() || name.find('=', 1) != std::string::npos || has_nul(name) ||
                has_nul(value)) {
                return std::unexpected(std::string(ARGUMENTS_NOT_VALID));
            }
        }
        std::vector<std::string> argv{program};
        argv.insert(argv.end(), spec.args.begin(), spec.args.end());
        std::wstring command = widen(windows_command_line(argv));
        std::wstring block = environment_block(env);
        const std::wstring directory = widen(cwd);

        const bool own_job = spec.own_tree && !spec.new_console;
        wil::unique_handle job;
        if (own_job) {
            std::uint32_t error = 0;
            job.reset(process_win::create_kill_on_close_job(error));
            if (!job) {
                return std::unexpected(process_detail::start_error(error));
            }
        }

        const bool redirect =
            !spec.new_console && (spec.pipes || spec.stdin_null || spec.stderr_null);
        wil::unique_handle child_in, child_out, child_err;
        wil::unique_handle parent_in, parent_out, parent_err;
        if (redirect) {
            bool ok = true;
            if (spec.stdin_null) {
                child_in = open_nul(GENERIC_READ);
            } else if (spec.pipes) {
                ok = ok && make_pipe(child_in, parent_in, true);
            } else {
                child_in = inherited_standard(STD_INPUT_HANDLE, GENERIC_READ);
            }
            if (spec.pipes) {
                ok = ok && make_pipe(parent_out, child_out, false);
            } else {
                child_out = inherited_standard(STD_OUTPUT_HANDLE, GENERIC_WRITE);
            }
            if (spec.stderr_null) {
                child_err = open_nul(GENERIC_WRITE);
            } else if (spec.pipes) {
                ok = ok && make_pipe(parent_err, child_err, false);
            } else {
                child_err = inherited_standard(STD_ERROR_HANDLE, GENERIC_WRITE);
            }
            if (!ok || !child_in || !child_out || !child_err) {
                return std::unexpected(process_detail::start_error(GetLastError()));
            }
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        std::vector<char> attribute_storage;
        std::vector<HANDLE> inherited;
        DWORD flags = CREATE_UNICODE_ENVIRONMENT;
        flags |= spec.new_console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
        if (own_job) {
            flags |= CREATE_SUSPENDED;
        }
        if (spec.new_process_group) {
            flags |= CREATE_NEW_PROCESS_GROUP;
        }
        LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
        if (redirect) {
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = child_in.get();
            startup.StartupInfo.hStdOutput = child_out.get();
            startup.StartupInfo.hStdError = child_err.get();
            for (HANDLE handle : {child_in.get(), child_out.get(), child_err.get()}) {
                if (std::find(inherited.begin(), inherited.end(), handle) == inherited.end()) {
                    inherited.push_back(handle);
                }
            }
            // Only these handles reach the child: a pipe of a launch on another thread must
            // not, or its reader would never see the end of it.
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            attribute_storage.resize(size);
            attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
                return std::unexpected(process_detail::start_error(GetLastError()));
            }
            if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited.data(), inherited.size() * sizeof(HANDLE),
                                           nullptr, nullptr)) {
                const DWORD error = GetLastError();
                DeleteProcThreadAttributeList(attributes);
                return std::unexpected(process_detail::start_error(error));
            }
            startup.lpAttributeList = attributes;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }

        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, redirect ? TRUE : FALSE, flags, block.data(),
            directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
        const DWORD create_error = created ? 0 : GetLastError();
        if (attributes != nullptr) {
            DeleteProcThreadAttributeList(attributes);
        }
        child_in.reset();
        child_out.reset();
        child_err.reset();
        if (!created) {
            return std::unexpected(process_detail::start_error(create_error));
        }
        wil::unique_handle process(info.hProcess);
        wil::unique_handle thread(info.hThread);
        if (own_job) {
            // Assigned before it runs, so nothing it starts escapes the job; any failure closes
            // the job, which ends the child.
            if (!AssignProcessToJobObject(job.get(), process.get())) {
                const DWORD error = GetLastError();
                TerminateProcess(process.get(), 1);
                return std::unexpected(process_detail::start_error(error));
            }
            if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
                const DWORD error = GetLastError();
                TerminateJobObject(job.get(), 1);
                return std::unexpected(process_detail::start_error(error));
            }
        }
        thread.reset();
        return std::make_unique<WinProcess>(std::move(process), info.dwProcessId, std::move(job),
                                            std::move(parent_in), std::move(parent_out),
                                            std::move(parent_err), spec.disown_on_clean_exit);
    }
};

} // namespace

std::shared_ptr<ProcessLauncher> make_process_launcher() {
    return std::make_shared<WinLauncher>();
}

bool can_start_programs() {
    return true;
}

} // namespace fv
