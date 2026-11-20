#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>

#include "base/identity.h"

namespace fv {

// Keeps a data root stable while the app runs or restores (OpenWhisper's backup_startup.py).
// Normal processes hold a shared byte-range lock on <root>/<lease_file>; a pending restore needs
// it exclusively before replacing any file, so no other live process sees a half-restored
// database or keeps Windows file handles open. The OS releases the lock when the process dies
// (Windows only some time after it exits).
enum class LeaseMode { Shared, Exclusive };

// Another process holds an incompatible lease: "The application data is in use by another
// FioriVoce process." (any failure to take the lock reads as this, as in Python).
class DataLeaseBusy : public std::runtime_error {
public:
    using runtime_error::runtime_error;
};

// The data could not be made safe to use: the lock file is a symbolic link or not a file. The
// startup restore gate (M0.11, M3) raises it too, with its own sentences.
class StartupRestoreError : public std::runtime_error {
public:
    using runtime_error::runtime_error;
};

// Move-only; releases on destroy.
// Affinity: one thread at a time.
class DataLease {
public:
    DataLease() = default; // holds nothing
    // Creates the root and the lock file (one NUL byte when empty) and takes the lock without
    // waiting. Throws DataLeaseBusy, StartupRestoreError, or std::filesystem::filesystem_error
    // when the root or the file cannot be created or opened.
    static DataLease acquire(const std::filesystem::path& root, const AppIdentity&, LeaseMode);

    DataLease(DataLease&&) noexcept;
    DataLease& operator=(DataLease&&) noexcept;
    DataLease(const DataLease&) = delete;
    DataLease& operator=(const DataLease&) = delete;
    ~DataLease();

    // Unlocks and closes the file. An unlock failure is ignored: closing the file releases the
    // lock anyway.
    void release();
    bool held() const;
    const std::filesystem::path& root() const; // normalized as normalize_data_root does
    LeaseMode mode() const;

private:
    std::filesystem::path root_;
    std::intptr_t file_ = -1; // HANDLE on Windows, file descriptor elsewhere
    LeaseMode mode_ = LeaseMode::Shared;
};

// os.path.normcase(os.path.realpath(os.path.abspath(root))): absolute, symbolic links resolved
// as far as the path exists, and on Windows lowercased with backslashes. Two spellings of one
// data root normalize to the same path.
std::filesystem::path normalize_data_root(const std::filesystem::path& root);

} // namespace fv
