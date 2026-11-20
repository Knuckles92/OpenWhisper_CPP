#include "base/data_lease.h"

#include <format>
#include <string>
#include <system_error>
#include <utility>

#include "base/detail/home.h"
#include "base/detail/lease_file.h"
#include "base/utf8.h"

namespace fv {

namespace {

// realpath on Windows answers with the final path name; the "\\?\" form it may carry names the
// same file as the plain one, which is what Python returns too.
std::filesystem::path without_long_prefix(const std::filesystem::path& path) {
    const std::string text = path_to_utf8(path);
    if (text.starts_with(R"(\\?\UNC\)")) {
        return path_from_utf8("\\\\" + text.substr(8));
    }
    if (text.starts_with(R"(\\?\)") && text.size() >= 6 && text[5] == ':') {
        return path_from_utf8(text.substr(4));
    }
    return path;
}

} // namespace

std::filesystem::path normalize_data_root(const std::filesystem::path& root) {
    const auto absolute = path_from_utf8(detail::abspath(path_to_utf8(root)));
    std::error_code error;
    auto real = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        real = absolute;
    }
    real = path_from_utf8(detail::abspath(path_to_utf8(without_long_prefix(real))));
    return detail::normcase(real);
}

// backup_startup.py:65-108 DataLease.acquire.
DataLease DataLease::acquire(const std::filesystem::path& root, const AppIdentity& app,
                             LeaseMode mode) {
    DataLease lease;
    lease.root_ = normalize_data_root(root);
    lease.mode_ = mode;
    std::filesystem::create_directories(lease.root_);
    const auto path = lease.root_ / path_from_utf8(app.lease_file);
    std::error_code error;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error))) {
        throw StartupRestoreError("The application data lock is a symbolic link.");
    }
    const std::intptr_t file = detail::open_lease_file(path);
    if (!detail::lock_lease_file(file, mode == LeaseMode::Exclusive)) {
        detail::close_lease_file(file);
        throw DataLeaseBusy(
            std::format("The application data is in use by another {} process.", app.product));
    }
    lease.file_ = file;
    return lease;
}

DataLease::DataLease(DataLease&& other) noexcept
    : root_(std::move(other.root_)), file_(std::exchange(other.file_, detail::kNoFile)),
      mode_(other.mode_) {}

DataLease& DataLease::operator=(DataLease&& other) noexcept {
    if (this != &other) {
        release();
        root_ = std::move(other.root_);
        file_ = std::exchange(other.file_, detail::kNoFile);
        mode_ = other.mode_;
    }
    return *this;
}

DataLease::~DataLease() {
    release();
}

void DataLease::release() {
    const std::intptr_t file = std::exchange(file_, detail::kNoFile);
    if (file != detail::kNoFile) {
        detail::unlock_and_close_lease_file(file);
    }
}

bool DataLease::held() const {
    return file_ != detail::kNoFile;
}

const std::filesystem::path& DataLease::root() const {
    return root_;
}

LeaseMode DataLease::mode() const {
    return mode_;
}

} // namespace fv
