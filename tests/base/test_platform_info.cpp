// Host CPU architecture without WMI, ported from OpenWhisper's tests/test_settings.py. The
// kernel query (IsWow64Process2) is faked through HostMachineProbe, as the Python tests patch
// config._windows_native_machine.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <set>
#include <string>
#include <tuple>

#include "base/platform_info.h"
#include "base/testing/map_env_reader.h"

using fv::HostMachineProbe;
using fv::MapEnvReader;

namespace {

// platform.machine(): on Windows it queries WMI, which killed startup with 0xC000070A.
int uname_calls = 0;
int kernel_calls = 0;

std::string uname_must_not_run() {
    ++uname_calls;
    return "uname was called";
}

std::optional<std::string> kernel_must_not_run() {
    ++kernel_calls;
    return "the kernel was asked";
}

// Checks, at the end of each test, that neither forbidden source was asked.
struct ForbiddenSources {
    ForbiddenSources() { uname_calls = kernel_calls = 0; }
    ~ForbiddenSources() {
        CHECK(uname_calls == 0);
        CHECK(kernel_calls == 0);
    }
};

} // namespace

TEST_CASE("platform helpers skip the architecture query off their platform", "[test_settings]") {
    ForbiddenSources forbidden;
    // from
    // tests/test_settings.py::test_platform_helpers_skip_the_architecture_query_off_their_platform
    // The parakeet_*_supported half of the Python test belongs to M1.11 (the model catalog);
    // here: each platform asks only its own source.
    MapEnvReader env{{"PROCESSOR_ARCHITECTURE", "AMD64"}};
    CHECK(fv::host_machine(HostMachineProbe{
              .platform = "win32",
              .windows_native_machine = [] { return std::optional<std::string>("AMD64"); },
              .uname_machine = uname_must_not_run,
              .env = &env}) == "AMD64");
    CHECK(fv::host_machine(HostMachineProbe{.platform = "linux",
                                            .windows_native_machine = kernel_must_not_run,
                                            .uname_machine = [] { return std::string("x86_64"); },
                                            .env = &env}) == "x86_64");
    CHECK(fv::host_machine(HostMachineProbe{.platform = "darwin",
                                            .windows_native_machine = kernel_must_not_run,
                                            .uname_machine = [] { return std::string("arm64"); },
                                            .env = &env}) == "arm64");
}

TEST_CASE("windows architecture comes from the kernel not wmi", "[test_settings]") {
    ForbiddenSources forbidden;
    // from tests/test_settings.py::test_windows_architecture_comes_from_the_kernel_not_wmi
    // (parametrized native AMD64 / ARM64; the parakeet_default_supported and current_platform_tag
    // checks of each case move to M1.11 with the catalog)
    const std::string native = GENERATE(std::string("AMD64"), std::string("ARM64"));
    CAPTURE(native);
    MapEnvReader env{{"PROCESSOR_ARCHITECTURE", "AMD64"}}; // emulated x64 on ARM64 says this

    CHECK(fv::host_machine(HostMachineProbe{
              .platform = "win32",
              .windows_native_machine = [native] { return std::optional<std::string>(native); },
              .uname_machine = uname_must_not_run,
              .env = &env}) == native);
}

TEST_CASE("windows architecture falls back to cpython environment", "[test_settings]") {
    ForbiddenSources forbidden;
    // from tests/test_settings.py::test_windows_architecture_falls_back_to_cpython_environment
    using Row = std::tuple<std::optional<std::string>, std::optional<std::string>, std::string>;
    const auto [wow64, arch, expected] =
        GENERATE(Row{"AMD64", "x86", "AMD64"}, Row{std::nullopt, "ARM64", "ARM64"},
                 Row{std::nullopt, std::nullopt, ""});
    MapEnvReader env;
    if (wow64) {
        env.set("PROCESSOR_ARCHITEW6432", *wow64);
    }
    if (arch) {
        env.set("PROCESSOR_ARCHITECTURE", *arch);
    }

    CHECK(fv::host_machine(HostMachineProbe{
              .platform = "win32",
              .windows_native_machine = [] { return std::optional<std::string>(); },
              .uname_machine = uname_must_not_run,
              .env = &env}) == expected);
}

TEST_CASE("an empty kernel or WOW64 answer falls through, as Python's or does") {
    ForbiddenSources forbidden;
    MapEnvReader env{{"PROCESSOR_ARCHITEW6432", ""}, {"PROCESSOR_ARCHITECTURE", "x86"}};
    CHECK(fv::host_machine(HostMachineProbe{
              .platform = "win32",
              .windows_native_machine = [] { return std::optional<std::string>(""); },
              .uname_machine = uname_must_not_run,
              .env = &env}) == "x86");
}

TEST_CASE("windows kernel reports a known architecture", "[test_settings]") {
    // from tests/test_settings.py::test_windows_kernel_reports_a_known_architecture
#if defined(_WIN32)
    const auto machine = fv::windows_native_machine();
    REQUIRE(machine);
    CHECK(std::set<std::string>{"AMD64", "ARM64", "x86"}.contains(*machine));
    CHECK(fv::host_machine() == *machine);
#else
    SKIP("asks the Windows kernel");
#endif
}

TEST_CASE("image file machine values are named as platform.machine names them") {
    CHECK(fv::windows_machine_name(0x014C) == "x86");
    CHECK(fv::windows_machine_name(0x01C4) == "ARM");
    CHECK(fv::windows_machine_name(0x0200) == "ia64");
    CHECK(fv::windows_machine_name(0x8664) == "AMD64");
    CHECK(fv::windows_machine_name(0xAA64) == "ARM64");
    CHECK_FALSE(fv::windows_machine_name(0x1234));
}

TEST_CASE("the platform is named as sys.platform names it") {
#if defined(_WIN32)
    CHECK(fv::python_platform() == "win32");
#elif defined(__APPLE__)
    CHECK(fv::python_platform() == "darwin");
#else
    CHECK(fv::python_platform() == "linux");
#endif
}
