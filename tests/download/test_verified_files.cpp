// File digests, free space and lock retries (services/verified_files.py).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/hash.h"
#include "base/log.h"
#include "base/testing/fake_clock.h"
#include "download/verified_files.h"
#include "golden/golden.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

std::system_error windows_error(int code) {
    return std::system_error(code, std::system_category(), "rename");
}

class CaptureSink final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        if (record.logger == "services.verified_files") {
            messages.emplace_back(record.message);
        }
    }
    std::vector<std::string> lines() {
        std::lock_guard lock(mutex_);
        return messages;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> messages;
};

} // namespace

TEST_CASE("sha256_file hashes the whole file", "[verified_files]") {
    const fv::testkit::TempDataRoot root;
    const std::string content = std::string(3 << 20, 'a') + "tail";
    write_file(root.path() / "big.bin", content);
    CHECK(fv::download::sha256_file(root.path() / "big.bin") == fv::sha256_hex(content));
    CHECK(fv::download::sha256_file(root.path() / "big.bin", true) == fv::sha256_hex(content));
    write_file(root.path() / "empty.bin", "");
    CHECK(fv::download::sha256_file(root.path() / "empty.bin", true) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_THROWS_AS(fv::download::sha256_file(root.path() / "missing.bin"), std::system_error);
}

TEST_CASE("sha256_file with regular_only refuses links", "[verified_files][platform]") {
    const fv::testkit::TempDataRoot root;
    write_file(root.path() / "real.bin", "payload");
    fs::create_hard_link(root.path() / "real.bin", root.path() / "linked.bin");
    CHECK_THROWS_AS(fv::download::sha256_file(root.path() / "linked.bin", true),
                    fv::download::NotARegularFile);
    // Without the flag a hard link is just the file.
    CHECK(fv::download::sha256_file(root.path() / "linked.bin") == fv::sha256_hex("payload"));

    std::error_code error;
    fs::create_symlink(root.path() / "real.bin", root.path() / "symlink.bin", error);
    if (!error) {
        CHECK_THROWS_AS(fv::download::sha256_file(root.path() / "symlink.bin", true),
                        fv::download::NotARegularFile);
    }
    fs::create_directories(root.path() / "folder");
    CHECK_THROWS(fv::download::sha256_file(root.path() / "folder", true));
}

TEST_CASE("free_space_shortfall matches Python's", "[golden][verified_files]") {
    fv::golden::for_each(
        "services.verified_files", "free_space_shortfall", [](const fv::golden::Case& c) {
            const fv::testkit::TempDataRoot root;
            const auto folder = root.path() / "space";
            const auto free = c.arg<std::uint64_t>(1);
            const auto margin = c.arg<std::optional<double>>(2);
            const auto free_bytes = [free](const fs::path&) {
                return free;
            };
            const auto shortfall = fv::download::free_space_shortfall(
                folder, c.arg<std::int64_t>(0), margin.value_or(fv::download::kSpaceMargin),
                free_bytes);
            const auto expected = c.result<std::optional<std::vector<std::int64_t>>>();
            REQUIRE(shortfall.has_value() == expected.has_value());
            if (expected) {
                CHECK(shortfall->first == (*expected)[0]);
                CHECK(shortfall->second == (*expected)[1]);
            }
        });
}

TEST_CASE("the free-space message matches Python's", "[golden][verified_files]") {
    fv::golden::for_each(
        "services.verified_files", "free_space_message", [](const fv::golden::Case& c) {
            CHECK(fv::download::describe_free_space_shortfall(
                      c.arg<std::int64_t>(0), c.arg<std::int64_t>(1)) == c.result<std::string>());
        });
}

TEST_CASE("free_space_shortfall creates the folder first and reads the real drive",
          "[verified_files][platform]") {
    const fv::testkit::TempDataRoot root;
    const auto folder = root.path() / "a" / "b";
    CHECK_FALSE(fv::download::free_space_shortfall(folder, 1));
    CHECK(fs::is_directory(folder));
    CHECK(fv::download::disk_free_bytes(folder) > 0);
    CHECK_FALSE(fv::download::free_space_shortfall(root.path() / "c", 0));
    CHECK_FALSE(fs::exists(root.path() / "c"));
}

TEST_CASE("only access, sharing and lock violations are lock errors on Windows",
          "[verified_files]") {
    const bool windows =
#ifdef _WIN32
        true;
#else
        false;
#endif
    for (const int code : {5, 32, 33}) {
        CHECK(fv::download::is_windows_lock_error(std::error_code(code, std::system_category())) ==
              windows);
    }
    CHECK_FALSE(fv::download::is_windows_lock_error(std::error_code(2, std::system_category())));
    CHECK_FALSE(
        fv::download::is_windows_lock_error(std::make_error_code(std::errc::permission_denied)));
}

TEST_CASE("retry_while_locked retries a lock until it clears", "[verified_files]") {
    fv::FakeClock clock;
    std::vector<std::chrono::milliseconds> waits;
    fv::download::LockRetryOptions options;
    options.clock = &clock;
    options.locked = [](const std::system_error& error) {
        return error.code().value() == 32;
    };
    options.wait = [&](std::chrono::milliseconds poll) {
        waits.push_back(poll);
        clock.advance(poll);
    };
    auto capture = std::make_shared<CaptureSink>();
    fv::log::add_sink(capture);

    int calls = 0;
    const auto value = fv::download::retry_while_locked(
        "C:\\runtime",
        [&] {
            if (++calls < 3) {
                throw windows_error(32);
            }
            return 7;
        },
        1s, options);
    CHECK(value == 7);
    CHECK(calls == 3);
    CHECK(waits == std::vector<std::chrono::milliseconds>{250ms, 250ms});
    const auto lines = capture->lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].starts_with("C:\\runtime is locked (["));
    CHECK(lines[0].ends_with("); retrying"));

    // Another error, or a lock that outlasts the timeout, comes through.
    calls = 0;
    CHECK_THROWS_AS(fv::download::retry_while_locked(
                        "x",
                        [&]() -> int {
                            ++calls;
                            throw windows_error(2);
                        },
                        1s, options),
                    std::system_error);
    CHECK(calls == 1);
    calls = 0;
    CHECK_THROWS_AS(fv::download::retry_while_locked(
                        "x",
                        [&]() -> int {
                            ++calls;
                            throw windows_error(32);
                        },
                        1s, options),
                    std::system_error);
    CHECK(calls == 5);
    fv::log::remove_sink(capture);
}

TEST_CASE("OS errors are worded as Python words them", "[verified_files]") {
#ifdef _WIN32
    CHECK(fv::download::os_error_text(std::error_code(32, std::system_category())) ==
          "[WinError 32] The process cannot access the file because it is being used by another "
          "process");
    const fs::filesystem_error error("rename", fs::path("C:\\a b\\x"), fs::path("C:\\y"),
                                     std::error_code(5, std::system_category()));
    CHECK(fv::download::os_error_text(error) ==
          "[WinError 5] Access is denied: 'C:\\\\a b\\\\x' -> 'C:\\\\y'");
#endif
    CHECK(fv::download::os_error_text(std::make_error_code(std::errc::no_space_on_device))
              .starts_with("[Errno "));
}

#ifdef _WIN32
TEST_CASE("a rename blocked by an open file succeeds once it closes",
          "[verified_files][platform]") {
    const fv::testkit::TempDataRoot root;
    const auto source = root.path() / "new.bin";
    const auto target = root.path() / "renamed.bin";
    write_file(source, "payload");
    auto holder = std::make_unique<std::ifstream>(source, std::ios::binary);
    REQUIRE(holder->is_open());
    std::thread release([&] {
        std::this_thread::sleep_for(400ms);
        holder.reset();
    });
    int attempts = 0;
    fv::download::LockRetryOptions options;
    options.poll = 100ms;
    fv::download::retry_while_locked(
        "new.bin",
        [&] {
            ++attempts;
            fs::rename(source, target);
        },
        5s, options);
    release.join();
    CHECK(attempts > 1);
    CHECK(fs::exists(target));
}
#endif
