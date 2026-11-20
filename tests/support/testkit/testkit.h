#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// fv_testkit: what every test directory links (architecture 9.1). Qt-free, so the core test
// executables build without Qt; the offscreen QApplication main is fv_testkit_qt.
namespace fv::testkit {

// Folders of the checkout and the build tree the tests were configured from.
std::filesystem::path source_root();
std::filesystem::path golden_path();                        // tests/golden
std::filesystem::path golden_path(std::string_view module); // tests/golden/<module>.json
std::filesystem::path reference_path();                     // tests/reference
std::filesystem::path parity_reference_file();              // tools/parity/reference.json
// <build>/test-artifacts, created on first use: diff images and other output a failing test
// leaves for a person to look at.
std::filesystem::path artifact_path();

// The process environment by UTF-8 name and value. set_env with nullopt removes the variable.
std::optional<std::string> get_env(std::string_view name);
void set_env(std::string_view name, const std::optional<std::string>& value);

// A fresh, empty data folder for the lifetime of the object, named by OPENWHISPER_CPP_DATA_DIR (the
// variable is spelled here, not taken from identity(), so testkit needs nothing fv_base
// implements). The previous value comes back and the folder is removed on destruction.
class TempDataRoot {
public:
    TempDataRoot();
    ~TempDataRoot();
    TempDataRoot(const TempDataRoot&) = delete;
    TempDataRoot& operator=(const TempDataRoot&) = delete;

    const std::filesystem::path& path() const;

private:
    std::filesystem::path path_;
    std::optional<std::string> previous_;
};

// Fails the running test when the guarded scope allocates with operator new on the thread that
// made the guard: the audio callback and the other real-time paths must not. Enforced in builds
// without NDEBUG (enforced() says which); elsewhere it counts nothing. Allocations inside DLLs
// that use their own operator new (Qt's) are not seen. Nesting is allowed.
class RealtimeGuard {
public:
    RealtimeGuard();
    // fail_on_allocation false: only count, for code that reports allocations() itself.
    explicit RealtimeGuard(bool fail_on_allocation);
    ~RealtimeGuard();
    RealtimeGuard(const RealtimeGuard&) = delete;
    RealtimeGuard& operator=(const RealtimeGuard&) = delete;

    static bool enforced();
    // Allocations so far in the guarded scope.
    std::size_t allocations() const;

private:
    std::size_t start_;
    bool fail_;
};

} // namespace fv::testkit
