#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// The message a failed update leaves for the next launch (services/app_update_apply.py:
// write_apply_error, consume_apply_error). The native updater (M10) writes it from a process
// without a window; the app shows it once, in an "Update failed" box.
namespace fv::update {

// A native update that cannot go on (services/app_update_apply.py: UpdateApplyError). The message
// is a full sentence: the app shows it, or the updater leaves it for the next launch.
class UpdateApplyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// This install or this release cannot take the native path and needs the setup package
// (UpdateRequiresSetup). apply_update answers it with the verified setup when one is ready.
class UpdateRequiresSetup : public UpdateApplyError {
public:
    using UpdateApplyError::UpdateApplyError;
};

// The user canceled while the update was being prepared (UpdateCanceled).
class UpdateCanceled : public UpdateApplyError {
public:
    using UpdateApplyError::UpdateApplyError;
};

// Characters (code points) kept of a message.
inline constexpr std::size_t kApplyErrorLimit = 2000;

// <updates_dir>/apply_error.txt
std::filesystem::path apply_error_path(const std::filesystem::path& updates_dir);

// Writes the message stripped and cut to kApplyErrorLimit ("The update failed." for an empty
// one), with a line ending, in text mode (CRLF on Windows), creating the folder. Throws
// std::filesystem::filesystem_error or std::system_error when it cannot.
void write_apply_error(const std::filesystem::path& updates_dir, std::string_view message);

// Reads and deletes the message: nullopt when there is none, it cannot be read (the file is then
// left in place), or it is blank. Lines are read as Python's text mode reads them (CRLF and CR
// as LF); the first 2,001 characters are stripped and cut to kApplyErrorLimit. A file that is not
// UTF-8 cannot be read. Blocking (a small file).
std::optional<std::string> consume_apply_error(const std::filesystem::path& updates_dir);

} // namespace fv::update
