#pragma once

// Another process holding a data lease, through lease_helper (fv_backup_lease_helper).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "test_support.h"

namespace fv::backup_test {

// A process holding the shared lease on `data_dir` (OpenWhisper's lease when `legacy`, as a
// running OpenWhisper holds it) until its standard input closes. Its destructor stops it.
inline std::unique_ptr<fv::test::ChildProcess> lease_holder(const std::filesystem::path& data_dir,
                                                            bool legacy = false) {
    std::vector<std::string> args{fv::path_to_utf8(data_dir)};
    if (legacy) {
        args.emplace_back("legacy");
    }
    auto process =
        std::make_unique<fv::test::ChildProcess>(fv::path_from_utf8(FV_BACKUP_LEASE_HELPER), args);
    REQUIRE(process->read_line(std::chrono::seconds(10)) == std::optional<std::string>("READY"));
    return process;
}

// Closing stdin lets the helper unlock the file itself; killing it would leave the unlock to
// Windows, which releases a dead process's locks some time after it exits.
inline void stop(fv::test::ChildProcess& process) {
    process.close_stdin();
    if (!process.wait(std::chrono::seconds(10))) {
        process.kill();
    }
    REQUIRE(process.wait(std::chrono::seconds(10)));
}

} // namespace fv::backup_test
