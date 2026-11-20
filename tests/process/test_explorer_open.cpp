// Links on Windows open through Explorer: OpenWhisper's tests/test_automation_r3c_launch.py and
// tests/test_automation_r4b_launch.py. Explorer is FakeLaunchBackend, or process_win's
// explorer_open over an in-process IDispatch; only the [interactive] tests reach the real shell
// (they register a URL scheme under HKCU\Software\Classes and start fv_process_child through
// Explorer, so the owner runs them).

#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/identity.h"
#include "base/json.h"
#include "process/detail/launch_internal.h"
#include "process/launch.h"
#include "process/testing/fake_launch_backend.h"
#include "process_test_support.h"
#include "testkit/testkit.h"

#ifdef _WIN32
#include "process/win/explorer_open.h"

#include <objbase.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shobjidl.h>
#endif

using fv::AssocString;
using fv::FakeLaunchBackend;
using fv::LaunchError;
using fv::NoAnswer;
using fv::process_test::ScopedLaunchBackend;

namespace {

constexpr std::uint32_t kFailed = 0x80004005;

// Every lookup "can't tell", so open_uri hands every link on.
std::shared_ptr<FakeLaunchBackend> undecided_backend() {
    auto backend = std::make_shared<FakeLaunchBackend>();
    backend->set_assoc_function(
        [](std::string_view, AssocString) { return fv::AssocAnswer{kFailed, ""}; });
    return backend;
}

// Stands in for Explorer: answers `opens` once told to (test_automation_r4b_launch.SlowExplorer).
struct SlowExplorer {
    std::mutex mutex;
    std::condition_variable changed;
    bool answer = false;
    bool done = false;
    bool opens = true;
    std::vector<std::string> asked;

    bool operator()(std::string_view file) {
        std::unique_lock lock(mutex);
        asked.emplace_back(file);
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return answer; });
        done = true;
        changed.notify_all();
        return opens;
    }
    void release() {
        std::lock_guard lock(mutex);
        answer = true;
        changed.notify_all();
    }
    bool wait_done() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
    }
    std::vector<std::string> asked_so_far() {
        std::lock_guard lock(mutex);
        return asked;
    }
};

// test_automation_r4b_launch's `explorer` fixture: a slow Explorer, 0.3 s of patience, and the
// links opened from this process instead. At the end Explorer answers and its turn is free.
struct SlowFixture {
    std::shared_ptr<FakeLaunchBackend> backend = undecided_backend();
    std::shared_ptr<SlowExplorer> explorer = std::make_shared<SlowExplorer>();
    ScopedLaunchBackend installed{backend};

    SlowFixture() {
        backend->set_explorer_wait_s(0.3);
        backend->set_explorer(
            [slow = explorer](std::string_view file, std::string_view) { return (*slow)(file); });
    }
    ~SlowFixture() {
        explorer->release();
        explorer->wait_done();
        // The turn is free again once a quick Explorer answers within five seconds.
        backend->set_explorer_wait_s(5.0);
        backend->set_explorer([](std::string_view, std::string_view) { return true; });
        try {
            fv::process_detail::ask_explorer("s:teardown", "");
        } catch (...) {
        }
    }
};

} // namespace

// -- the opener, with Explorer faked (tests/test_automation_r3c_launch.py) -------------------

// from tests/test_automation_r3c_launch.py::test_every_link_goes_to_explorer_as_it_is
TEST_CASE("every link goes to explorer as it is", "[test_automation_r3c_launch]") {
    std::string long_link = "https://chatgpt.com/?q=";
    for (int i = 0; i < 6000; ++i) {
        long_link += "a%20";
    }
    const auto uri = GENERATE_COPY(values<std::string>({
        "claude-cli://open?q=hello%20there",
        "ms-mmsys:",                    // a rundll32 handler
        "proton-vpn://connect",         // an unquoted program path with spaces
        "codex://new?prompt=caf%C3%A9", // a packaged app
        long_link,
        "s:caf\xC3\xA9/\xE4\xB8\xAD\xE6\x96\x87?q=\xC3\xBC\xF0\x9F\x98\x80", // not ASCII
        "s:path\\",                                                          // ends in a backslash
    }));
    auto backend = undecided_backend();
    ScopedLaunchBackend installed(backend);
    fv::open_uri(uri);
    const auto opened = backend->explorer_opened();
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].first == uri);
    CHECK(opened[0].second.empty());
    CHECK(backend->opened_here().empty());
}

// from tests/test_automation_r3c_launch.py::test_without_explorers_desktop_the_link_opens_from_here
TEST_CASE("without explorer's desktop the link opens from here", "[test_automation_r3c_launch]") {
    auto backend = undecided_backend();
    ScopedLaunchBackend installed(backend);
    backend->set_explorer([](std::string_view, std::string_view) { return false; });
    fv::open_uri("codex://new?prompt=hi");
    const auto here = backend->opened_here();
    REQUIRE(here.size() == 1);
    CHECK(here[0].first == "codex://new?prompt=hi");
}

// from tests/test_automation_r3c_launch.py::test_when_nothing_opens_it_says_so
TEST_CASE("when nothing opens it says so", "[test_automation_r3c_launch]") {
    auto backend = undecided_backend();
    ScopedLaunchBackend installed(backend);
    backend->set_explorer([](std::string_view, std::string_view) { return false; });
    backend->set_start_here([](std::string_view, std::string_view) { return false; });
    try {
        fv::open_uri("odd:x");
        FAIL("opened");
    } catch (const LaunchError& error) {
        CHECK(std::string(error.what()) == "No app on this computer opens that link.");
    }
}

// from tests/test_automation_r3c_launch.py::test_a_rundll32_handler_counts
TEST_CASE("a rundll32 handler counts", "[test_automation_r3c_launch]") {
    // ASSOCSTR_EXECUTABLE names the DLL: it exists, so the link can open.
    auto backend = std::make_shared<FakeLaunchBackend>();
    ScopedLaunchBackend installed(backend);
    backend->set_assoc(
        "ms-mmsys", AssocString::command, 0,
        R"(C:\WINDOWS\system32\rundll32.exe shell32.dll,Control_RunDLL mmsys.cpl,,0)");
    backend->set_assoc("ms-mmsys", AssocString::executable, 0,
                       R"(C:\WINDOWS\system32\shell32.dll)");
    CHECK(fv::handler_available("ms-mmsys") == true);
}

#ifdef _WIN32

namespace {

// Explorer's desktop Shell.Application, in this process: records what ShellExecute is invoked
// with (test_explorer_is_asked_with_the_default_verb's Application).
class RecordingApplication final : public IDispatch {
public:
    struct Call {
        DISPID id;
        WORD flags;
        std::vector<std::string> strings;
        long show;
    };
    std::vector<Call> calls;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IDispatch) {
            *out = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID,
                                            DISPID* ids) override {
        if (count == 1 && std::wstring_view(names[0]) == L"ShellExecute") {
            ids[0] = 7;
            return S_OK;
        }
        return DISP_E_UNKNOWNNAME;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS* parameters,
                                     VARIANT*, EXCEPINFO*, UINT*) override {
        Call call{id, flags, {}, 0};
        // DISPPARAMS hold the arguments last first.
        for (UINT i = parameters->cArgs; i-- > 0;) {
            const VARIANT& value = parameters->rgvarg[i];
            if (value.vt == VT_BSTR) {
                call.strings.push_back(fv::process_win::narrow(
                    std::wstring_view(value.bstrVal, SysStringLen(value.bstrVal))));
            } else if (value.vt == VT_I4) {
                call.show = value.lVal;
            }
        }
        calls.push_back(std::move(call));
        return S_OK;
    }

private:
    ULONG references_ = 1;
};

// Runs `body` on a thread of its own with COM, as an action's worker opens links.
template <class F> auto in_thread(F body) {
    std::optional<decltype(body())> result;
    std::exception_ptr error;
    std::thread([&] {
        fv::process_detail::ComScope com;
        try {
            result = body();
        } catch (...) {
            error = std::current_exception();
        }
    }).join();
    if (error) {
        std::rethrow_exception(error);
    }
    return *result;
}

} // namespace

// from tests/test_automation_r3c_launch.py::test_explorer_is_asked_with_the_default_verb
TEST_CASE("explorer is asked with the default verb", "[test_automation_r3c_launch]") {
    RecordingApplication application;
    std::vector<HWND> allowed;
    const auto window = reinterpret_cast<HWND>(static_cast<LONG_PTR>(65914));
    const bool opened = in_thread([&] {
        return fv::process_win::explorer_open(
            "s:x", "",
            [&]() -> std::optional<fv::process_win::DesktopApplication> {
                fv::process_win::DesktopApplication desktop;
                application.AddRef();
                desktop.application.attach(&application);
                desktop.window = window;
                return desktop;
            },
            [&](HWND hwnd) { allowed.push_back(hwnd); });
    });
    CHECK(opened);
    // File, no parameters, no folder, the default verb, SW_SHOWNORMAL.
    REQUIRE(application.calls.size() == 1);
    const auto& call = application.calls[0];
    CHECK(call.id == 7);
    CHECK(call.flags == DISPATCH_METHOD);
    CHECK(call.strings == std::vector<std::string>{"s:x", "", "", ""});
    CHECK(call.show == SW_SHOWNORMAL);
    CHECK(allowed == std::vector<HWND>{window});
}

// from
// tests/test_automation_r3c_launch.py::test_no_desktop_or_no_answer_from_explorer_means_not_opened
TEST_CASE("no desktop or no answer from explorer means not opened",
          "[test_automation_r3c_launch]") {
    bool allowed = false;
    const auto never = [&](HWND) {
        allowed = true;
    };
    // The RPC server is unavailable.
    CHECK_FALSE(in_thread([&] {
        return fv::process_win::explorer_open(
            "s:x", "",
            []() -> std::optional<fv::process_win::DesktopApplication> {
                throw fv::process_win::ComError(static_cast<HRESULT>(0x800706BA));
            },
            never);
    }));
    CHECK_FALSE(in_thread([&] {
        return fv::process_win::explorer_open(
            "s:x", "", [] { return std::optional<fv::process_win::DesktopApplication>(); }, never);
    }));
    CHECK_FALSE(allowed);
}

#endif // _WIN32

// from
// tests/test_automation_r3c_launch.py::test_the_checks_that_drive_the_real_shell_run_only_when_asked
// The default test presets exclude the label interactive; this checks the two real-shell tests
// carry it.
TEST_CASE("the checks that drive the real shell run only when asked",
          "[test_automation_r3c_launch]") {
    const std::vector<std::string> real_shell{
        "what a link opens gets explorer's environment, not the app's",
        "a link reaches its handler as shellexecute passes it",
    };
    std::size_t found = 0;
    for (const auto& handle : Catch::getRegistryHub().getTestCaseRegistry().getAllTests()) {
        const auto* info = &handle.getTestCaseInfo();
        if (std::find(real_shell.begin(), real_shell.end(), info->name) == real_shell.end()) {
            continue;
        }
        ++found;
        const bool interactive =
            std::any_of(info->tags.begin(), info->tags.end(), [](const auto& tag) {
                return tag.original == Catch::StringRef("interactive");
            });
        CHECK(interactive);
    }
#ifdef _WIN32
    CHECK(found == real_shell.size());
#endif
}

// -- test schemes never outlive their test (tests/test_automation_r3c_launch.py) -----------

namespace {

constexpr std::string_view kClasses = "Software\\Classes";
// A test scheme lives for seconds; one this old was left by a killed run.
constexpr auto kStaleScheme = std::chrono::seconds(600);

std::string scheme_label() {
    return std::format("URL:{} test", fv::identity().product);
}

// The part of HKCU the scheme helpers use.
class Registry {
public:
    virtual ~Registry() = default;
    virtual void create_key(const std::string& path) = 0;
    virtual void set_value(const std::string& path, const std::string& name,
                           const std::string& value) = 0;
    virtual std::optional<std::string> value(const std::string& path, const std::string& name) = 0;
    virtual std::vector<std::string> subkeys(const std::string& path) = 0;
    // False when it doesn't exist or still has subkeys (as Windows: subkeys first).
    virtual bool delete_key(const std::string& path) = 0;
    virtual std::optional<std::chrono::system_clock::time_point>
    written(const std::string& path) = 0;
};

void delete_tree(Registry& registry, const std::string& path) {
    for (const auto& child : registry.subkeys(path)) {
        delete_tree(registry, path + "\\" + child);
    }
    registry.delete_key(path);
}

bool test_scheme_name(const std::string& name) {
    static const std::regex pattern("fvtest[0-9a-f]{12}");
    return std::regex_match(name, pattern);
}

// Removes test schemes a killed run left behind; never one a running test uses.
void remove_stale_schemes(Registry& registry) {
    for (const auto& name : registry.subkeys(std::string(kClasses))) {
        if (!test_scheme_name(name)) {
            continue;
        }
        const auto path = std::string(kClasses) + "\\" + name;
        const auto label = registry.value(path, "");
        const auto written = registry.written(path);
        if (label == scheme_label() && written &&
            std::chrono::system_clock::now() - *written > kStaleScheme) {
            delete_tree(registry, path);
        }
    }
}

// A URL scheme run by `command` for one test; removed however the test ends.
class TestScheme {
public:
    TestScheme(Registry& registry, const std::string& command) : registry_(registry) {
        remove_stale_schemes(registry_);
        std::mt19937_64 random(std::random_device{}());
        name_ = std::format("fvtest{:012x}", random() & 0xFFFFFFFFFFFFull);
        const auto base = std::string(kClasses) + "\\" + name_;
        registry_.create_key(base);
        registry_.set_value(base, "", scheme_label());
        registry_.set_value(base, "URL Protocol", "");
        registry_.create_key(base + "\\shell\\open\\command");
        registry_.set_value(base + "\\shell\\open\\command", "", command);
    }
    ~TestScheme() {
        delete_tree(registry_, std::string(kClasses) + "\\" + name_);
        fv::forget_handlers();
    }
    TestScheme(const TestScheme&) = delete;
    TestScheme& operator=(const TestScheme&) = delete;
    const std::string& name() const { return name_; }

private:
    Registry& registry_;
    std::string name_;
};

// HKCU in memory (test_automation_r3c_launch.FakeRegistry).
class FakeRegistry final : public Registry {
public:
    FakeRegistry() { create_key(std::string(kClasses)); }

    void create_key(const std::string& path) override {
        std::string partial;
        std::size_t start = 0;
        for (;;) {
            const auto end = path.find('\\', start);
            partial = path.substr(0, end);
            if (!keys_.contains(partial)) {
                keys_[partial];
                written_[partial] = std::chrono::system_clock::now();
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    void set_value(const std::string& path, const std::string& name,
                   const std::string& value) override {
        keys_.at(path)[name] = value;
        written_[path] = std::chrono::system_clock::now();
    }
    std::optional<std::string> value(const std::string& path, const std::string& name) override {
        const auto key = keys_.find(path);
        if (key == keys_.end() || !key->second.contains(name)) {
            return std::nullopt;
        }
        return key->second.at(name);
    }
    std::vector<std::string> subkeys(const std::string& path) override {
        std::vector<std::string> names;
        const auto prefix = path + "\\";
        for (const auto& [key, values] : keys_) {
            if (key.starts_with(prefix)) {
                const auto child =
                    key.substr(prefix.size(), key.find('\\', prefix.size()) - prefix.size());
                if (std::find(names.begin(), names.end(), child) == names.end()) {
                    names.push_back(child);
                }
            }
        }
        return names;
    }
    bool delete_key(const std::string& path) override {
        if (!keys_.contains(path) || !subkeys(path).empty()) {
            return false;
        }
        keys_.erase(path);
        return true;
    }
    std::optional<std::chrono::system_clock::time_point> written(const std::string& path) override {
        const auto it = written_.find(path);
        if (it == written_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<std::string> schemes() {
        std::vector<std::string> names;
        for (const auto& name : subkeys(std::string(kClasses))) {
            if (name.starts_with("fvtest")) {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }
    void left_behind(const std::string& name, const std::string& label, std::chrono::seconds age) {
        const auto path = std::string(kClasses) + "\\" + name;
        create_key(path);
        set_value(path, "", label);
        create_key(path + "\\shell\\open\\command");
        written_[path] = std::chrono::system_clock::now() - age;
    }

private:
    std::map<std::string, std::map<std::string, std::string>> keys_;
    std::map<std::string, std::chrono::system_clock::time_point> written_;
};

struct Interrupted {};

} // namespace

// from tests/test_automation_r3c_launch.py::test_a_test_scheme_is_removed_however_the_test_ends
TEST_CASE("a test scheme is removed however the test ends", "[test_automation_r3c_launch]") {
    FakeRegistry registry;
    CHECK_THROWS_AS(
        [&] {
            TestScheme scheme(registry, "handler %1");
            CHECK(registry.schemes() == std::vector<std::string>{scheme.name()});
            registry.create_key(std::string(kClasses) + "\\" + scheme.name() +
                                "\\shell\\open\\ddeexec");
            throw Interrupted{};
        }(),
        Interrupted);
    CHECK(registry.schemes().empty());
}

// from
// tests/test_automation_r3c_launch.py::test_schemes_a_killed_run_left_behind_go_before_the_next_is_registered
TEST_CASE("schemes a killed run left behind go before the next is registered",
          "[test_automation_r3c_launch]") {
    FakeRegistry registry;
    const std::string killed = "fvtest" + std::string(12, 'a');  // an hour old: killed run
    const std::string running = "fvtest" + std::string(12, 'b'); // another test's, running
    const std::string other = "fvtest" + std::string(12, 'c');   // not ours
    registry.left_behind(killed, scheme_label(), std::chrono::hours(1));
    registry.left_behind(running, scheme_label(), std::chrono::seconds(5));
    registry.left_behind(other, "URL:Other", std::chrono::hours(1));
    TestScheme scheme(registry, "handler %1");
    std::vector<std::string> expected{running, other, scheme.name()};
    std::sort(expected.begin(), expected.end());
    CHECK(registry.schemes() == expected);
}

#ifdef _WIN32

namespace {

// HKCU itself.
class WindowsRegistry final : public Registry {
public:
    void create_key(const std::string& path) override {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, fv::process_win::widen(path).c_str(), 0, nullptr, 0,
                            KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            RegCloseKey(key);
        }
    }
    void set_value(const std::string& path, const std::string& name,
                   const std::string& value) override {
        const auto wide = fv::process_win::widen(value);
        RegSetKeyValueW(HKEY_CURRENT_USER, fv::process_win::widen(path).c_str(),
                        fv::process_win::widen(name).c_str(), REG_SZ, wide.c_str(),
                        static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    }
    std::optional<std::string> value(const std::string& path, const std::string& name) override {
        wchar_t buffer[1024];
        DWORD size = sizeof(buffer);
        if (RegGetValueW(HKEY_CURRENT_USER, fv::process_win::widen(path).c_str(),
                         fv::process_win::widen(name).c_str(), RRF_RT_REG_SZ, nullptr, buffer,
                         &size) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        return fv::process_win::narrow(buffer);
    }
    std::vector<std::string> subkeys(const std::string& path) override {
        std::vector<std::string> names;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, fv::process_win::widen(path).c_str(), 0, KEY_READ,
                          &key) != ERROR_SUCCESS) {
            return names;
        }
        for (DWORD index = 0;; ++index) {
            wchar_t name[256];
            DWORD length = 256;
            if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            names.push_back(fv::process_win::narrow(std::wstring_view(name, length)));
        }
        RegCloseKey(key);
        return names;
    }
    bool delete_key(const std::string& path) override {
        return RegDeleteKeyW(HKEY_CURRENT_USER, fv::process_win::widen(path).c_str()) ==
               ERROR_SUCCESS;
    }
    std::optional<std::chrono::system_clock::time_point> written(const std::string& path) override {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, fv::process_win::widen(path).c_str(), 0, KEY_READ,
                          &key) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        FILETIME time{};
        const auto result = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr, nullptr, &time);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) {
            return std::nullopt;
        }
        const auto ticks =
            (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        // 100 ns ticks from 1601-01-01 to the Unix epoch.
        constexpr std::uint64_t epoch_1601 = 116444736000000000ull;
        return std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::microseconds((ticks - epoch_1601) / 10)));
    }
};

// What fv_process_child's `record` command wrote, one JSON object per line.
std::vector<fv::Json> records(const std::filesystem::path& log, std::size_t count) {
    std::vector<fv::Json> out;
    fv::process_test::wait_until(
        [&] {
            out.clear();
            std::istringstream lines(fv::process_test::read_file(log));
            for (std::string line; std::getline(lines, line);) {
                if (!line.empty()) {
                    out.push_back(fv::Json::parse(line));
                }
            }
            return out.size() >= count;
        },
        std::chrono::seconds(30));
    REQUIRE(out.size() >= count);
    return out;
}

// ShellExecuteExW in this process, never with any UI: the reference.
bool shell_execute_here_without_ui(const std::string& uri) {
    const auto wide = fv::process_win::widen(uri);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpFile = wide.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

std::string mark_name() {
    return std::string(fv::identity().env_prefix) + "TEST_LAUNCH_MARK";
}

} // namespace

// from
// tests/test_automation_r3c_launch.py::test_what_a_link_opens_gets_explorers_environment_not_openwhispers
TEST_CASE("what a link opens gets explorer's environment, not the app's",
          "[test_automation_r3c_launch][interactive]") {
    // The app a link opens starts from Explorer. ShellExecute in this process would hand it this
    // process's environment: .env keys, the runtime's folders on PATH.
    fv::set_launch_backend(nullptr);
    const bool has_desktop = in_thread([] {
        const auto desktop = fv::process_win::desktop_application();
        if (!desktop) {
            return false;
        }
        DWORD pid = 0;
        DWORD theirs = 0;
        DWORD ours = 0;
        GetWindowThreadProcessId(desktop->window, &pid);
        return ProcessIdToSessionId(pid, &theirs) &&
               ProcessIdToSessionId(GetCurrentProcessId(), &ours) && theirs == ours;
    });
    if (!has_desktop) {
        SKIP("Explorer's desktop isn't running");
    }
    fv::testkit::TempDataRoot root;
    const auto log = root.path() / "record.jsonl";
    const auto shortcut = root.path() / "recorder.lnk"; // opens like a link, unregistered
    in_thread([&] {
        const auto check = [](HRESULT hr) {
            if (FAILED(hr)) {
                throw fv::process_win::ComError(hr);
            }
        };
        wil::com_ptr_nothrow<IShellLinkW> link;
        check(
            CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)));
        link->SetPath(fv::path_from_utf8(fv::process_test::child()).c_str());
        const auto arguments = std::format(L"record \"{}\" {} opened", log.wstring(),
                                           fv::process_win::widen(mark_name()));
        link->SetArguments(arguments.c_str());
        wil::com_ptr_nothrow<IPersistFile> file;
        check(link->QueryInterface(IID_PPV_ARGS(&file)));
        check(file->Save(shortcut.c_str(), TRUE));
        return true;
    });
    fv::process_test::ScopedEnv mark(mark_name(), "from-the-app");

    const auto file = fv::path_to_utf8(shortcut);
    CHECK(fv::process_detail::ask_explorer(file, ""));
    const auto through_explorer = records(log, 1)[0];
    in_thread([&] { return fv::process_win::shell_execute_here(file, ""); });
    const auto from_here = records(log, 2)[1];

    CHECK(through_explorer.at("args") == fv::Json::array({"opened"}));
    CHECK(from_here.at("args") == fv::Json::array({"opened"}));
    CHECK(from_here.at("mark").at(0) == "from-the-app"); // what the old way passed on
    CHECK(through_explorer.at("mark").at(0) == "<unset>");
}

// from
// tests/test_automation_r3c_launch.py::test_a_link_reaches_its_handler_as_shellexecute_passes_it
TEST_CASE("a link reaches its handler as shellexecute passes it",
          "[test_automation_r3c_launch][interactive]") {
    // The Explorer path's ShellExecute call, made on this process's own Shell.Application
    // (Explorer can't see a scheme a test registers), against ShellExecuteExW: the same command
    // line each time.
    fv::testkit::TempDataRoot root;
    const auto log = root.path() / "record.jsonl";
    const auto spaced = root.path() / "child dir"; // a copy, so the program path has a space
    std::filesystem::create_directories(spaced);
    const auto child = fv::path_from_utf8(fv::process_test::child());
    std::filesystem::copy_file(child, spaced / child.filename());
    const auto log_text = fv::path_to_utf8(log);
    const auto [label, command] = GENERATE_COPY(table<std::string, std::string>({
        {"quoted",
         std::format("\"{}\" record \"{}\" mark \"%1\"", fv::process_test::child(), log_text)},
        {"link-unquoted",
         std::format("\"{}\" record \"{}\" mark %1", fv::process_test::child(), log_text)},
        {"program-path-unquoted",
         std::format("{} record \"{}\" mark \"%1\"", fv::path_to_utf8(spaced / child.filename()),
                     log_text)},
    }));
    CAPTURE(label);
    WindowsRegistry registry;
    TestScheme scheme(registry, command);
    std::string many = "{}://x?q=";
    for (int i = 0; i < 5000; ++i) {
        many += "a%20";
    }
    const std::vector<std::string> links{
        "{}://new?q=caf%C3%A9%20%22quoted%22&b=1#frag",
        "{}:caf\xC3\xA9/\xE4\xB8\xAD\xE6\x96\x87?q=\xC3\xBC\xF0\x9F\x98\x80",
        many, // 20,000 characters
    };
    std::size_t index = 0;
    for (const auto& link : links) {
        const auto uri = std::vformat(link, std::make_format_args(scheme.name()));
        CHECK(in_thread([&] {
            return shell_execute_here_without_ui(uri); // never any UI
        }));
        records(log, 2 * index + 1);
        const bool opened = in_thread([&] {
            return fv::process_win::explorer_open(
                uri, "",
                []() -> std::optional<fv::process_win::DesktopApplication> {
                    fv::process_win::DesktopApplication application;
                    const HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                                        IID_PPV_ARGS(&application.application));
                    if (FAILED(hr)) {
                        throw std::runtime_error("Shell.Application couldn't be created");
                    }
                    return application;
                },
                [](HWND) {});
        });
        CHECK(opened);
        const auto all = records(log, 2 * index + 2);
        const auto& reference = all[all.size() - 2];
        const auto& ours = all.back();
        CHECK(ours.at("cmd") == reference.at("cmd"));
        CHECK(ours.at("args") == reference.at("args"));
        CHECK(ours.at("args").back().get<std::string>().find(scheme.name()) != std::string::npos);
        ++index;
    }
}

#endif // _WIN32

// -- tests/test_automation_r4b_launch.py ---------------------------------------------------

// from
// tests/test_automation_r4b_launch.py::test_an_explorer_that_doesnt_answer_in_time_means_not_opened
TEST_CASE("an explorer that doesn't answer in time means not opened",
          "[test_automation_r4b_launch]") {
    SlowFixture f;
    f.explorer->opens = false; // a late "no desktop" doesn't open it from here either
    const auto started = std::chrono::steady_clock::now();
    try {
        fv::open_uri("s:x");
        FAIL("opened");
    } catch (const NoAnswer& error) {
        CHECK(std::string(error.what()) == "Windows didn't answer. Try again.");
    }
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));

    f.explorer->release();
    CHECK(f.explorer->wait_done());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(f.backend->opened_here().empty());
}

// from
// tests/test_automation_r4b_launch.py::test_no_more_links_queue_behind_one_explorer_hasnt_answered
TEST_CASE("no more links queue behind one explorer hasn't answered",
          "[test_automation_r4b_launch]") {
    SlowFixture f;
    CHECK_THROWS_AS(fv::open_uri("s:first"), NoAnswer);
    CHECK_THROWS_AS(fv::open_uri("s:second"), NoAnswer);
    CHECK(f.explorer->asked_so_far() == std::vector<std::string>{"s:first"});

    f.explorer->release();
    CHECK(f.explorer->wait_done());
    fv::open_uri("s:third"); // Explorer answers again: links go to it
    CHECK(f.explorer->asked_so_far() == std::vector<std::string>{"s:first", "s:third"});
    CHECK(f.backend->opened_here().empty());
}

// from tests/test_automation_r4b_launch.py::test_explorer_answering_in_time_opens_the_link_once
TEST_CASE("explorer answering in time opens the link once", "[test_automation_r4b_launch]") {
    SlowFixture f;
    f.explorer->release();
    fv::open_uri("s:x");
    CHECK(f.explorer->asked_so_far() == std::vector<std::string>{"s:x"});
    CHECK(f.backend->opened_here().empty());
}

// from tests/test_automation_r4b_launch.py::test_without_pywin32_a_link_opens_from_here
// OpenWhisper_CPP asks Explorer with no extra library; what stands for a missing pywin32 is an
// Explorer that can't be asked.
TEST_CASE("when explorer can't be asked a link opens from here", "[test_automation_r4b_launch]") {
    auto backend = undecided_backend();
    ScopedLaunchBackend installed(backend);
    backend->set_explorer([](std::string_view, std::string_view) { return false; });
    fv::open_uri("s:x");
    const auto here = backend->opened_here();
    REQUIRE(here.size() == 1);
    CHECK(here[0].first == "s:x");
}

// from tests/test_automation_r4b_launch.py::test_a_failure_while_asking_explorer_reaches_the_caller
TEST_CASE("a failure while asking explorer reaches the caller", "[test_automation_r4b_launch]") {
    auto backend = undecided_backend();
    ScopedLaunchBackend installed(backend);
    backend->set_explorer(
        [](std::string_view, std::string_view) -> bool { throw std::runtime_error("broken"); });
    try {
        fv::open_uri("s:x");
        FAIL("no failure");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "broken");
    }
}

#ifdef _WIN32
// from
// tests/test_automation_r4b_launch.py::test_opening_a_link_leaves_the_workers_thread_without_com
TEST_CASE("opening a link leaves the worker's thread without com", "[test_automation_r4b_launch]") {
    auto backend = undecided_backend();
    ScopedLaunchBackend installed(backend);
    std::optional<APTTYPE> explorer_apartment;
    backend->set_explorer([&](std::string_view, std::string_view) {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (SUCCEEDED(CoGetApartmentType(&type, &qualifier))) {
            explorer_apartment = type;
        }
        return true;
    });
    HRESULT worker_apartment = S_OK;
    std::thread([&] {
        fv::open_uri("s:x");
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        worker_apartment = CoGetApartmentType(&type, &qualifier);
    }).join();
    CHECK(worker_apartment == CO_E_NOTINITIALIZED); // not left a single-threaded apartment
    // A single-threaded apartment (the main one when it is the first in the process).
    REQUIRE(explorer_apartment.has_value());
    CHECK((*explorer_apartment == APTTYPE_STA || *explorer_apartment == APTTYPE_MAINSTA));
}
#endif
