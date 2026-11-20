#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "settings/settings_file.h"
#include "settings/store.h"

// The in-memory settings file other packages' tests use in place of the one on disk (the port
// of OpenWhisper's tests/fakes/settings.py InMemorySettings). The store over it behaves as the
// real one does (cache, strict writes, watchers, the same bytes), with nothing on disk.
namespace fv {

// Affinity: thread-safe.
class MemorySettingsFile final : public SettingsFile {
public:
    // nullopt: no file yet.
    explicit MemorySettingsFile(std::optional<std::string> bytes = std::nullopt);

    const std::filesystem::path& path() const override;
    std::optional<FileSignature> signature() const override;
    bool exists() const override;
    std::string read() const override;
    void replace(std::string_view bytes) override;

    // The file's bytes, or nullopt when there is none.
    std::optional<std::string> bytes() const;
    // An outside writer: replaces the bytes (nullopt removes the file) without the store.
    void set_bytes(std::optional<std::string> bytes);
    // When set, the next replace() throws std::runtime_error with this message, writing
    // nothing (an os.replace that failed).
    void fail_next_replace(std::string message);
    std::size_t reads() const;
    std::size_t writes() const;

private:
    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::optional<std::string> bytes_;
    std::uint64_t generation_ = 0;
    std::optional<std::string> failure_;
    mutable std::size_t reads_ = 0;
    std::size_t writes_ = 0;
};

// A SettingsStore over a MemorySettingsFile. By default the file holds `{}`: an existing
// install, as InMemorySettings is; pass std::nullopt for a brand-new install (no file).
class MemorySettingsStore final : public SettingsStore {
public:
    MemorySettingsStore();
    // The file holds `initial`, written as the store writes it.
    explicit MemorySettingsStore(const Json& initial);
    explicit MemorySettingsStore(std::nullopt_t);

    // What the file holds now, parsed ({} when there is no file).
    Json contents() const;
    // Writes `settings` as the store would, but as an outside writer: no watcher hears it.
    void set_contents(const Json& settings);
    MemorySettingsFile& file() const { return static_cast<MemorySettingsFile&>(backing_file()); }
};

} // namespace fv
