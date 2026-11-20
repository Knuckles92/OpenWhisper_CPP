#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/executor.h"
#include "base/subscription.h"
#include "settings/setting.h"
#include "settings/settings_file.h"

namespace fv {

// fiorivoce_settings.json: one JSON object, indent 2, trailing newline, insertion order kept,
// lines ended as Python's text mode ends them ("\r\n" on Windows), so the bytes are the ones
// OpenWhisper writes. Writes are atomic (temporary file, flush, replace). The port of
// services/settings.py's
// SettingsManager. Reads are tolerant: a missing file is {}, and a file that cannot be read or
// parsed is {} with the warning "Failed to load all settings: <error>". Read-modify-write
// (update, mutate) is strict: such a file throws SettingsFileError and is left as it was.
// Construct it after any pending restore and before anything writes the file, so
// is_new_install() can tell a brand-new install apart.
// Affinity: thread-safe. One lock covers each whole read-modify-write; watchers run after the
// replace, outside the lock.
class SettingsStore {
public:
    // paths.user_file(identity().settings_file) in the app.
    explicit SettingsStore(std::filesystem::path file);
    explicit SettingsStore(std::unique_ptr<SettingsFile> file);
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;
    virtual ~SettingsStore();

    // load_all_settings: the saved mapping, cached until the file's signature changes (an
    // outside edit or replace is picked up on the next call). Never throws.
    std::shared_ptr<const Json> snapshot() const; // copy-free
    template <class T> T get(const Setting<T>& setting) const { return setting.read(*snapshot()); }
    // load_settings: copies of the saved values of `keys` alone; an unsaved key is left out.
    Json load_settings(std::span<const std::string_view> keys) const;
    Json load_settings(std::initializer_list<std::string_view> keys) const;
    // SettingsManager.get(key, default): the saved value, unvalidated, or `fallback`.
    Json value(std::string_view key, const Json& fallback = nullptr) const;
    // A token that changes when the file does; nullopt when it cannot be checked.
    std::optional<FileSignature> file_signature() const;

    // save_all_settings: replaces the whole mapping (an object, else std::invalid_argument).
    void save_all(const Json& settings);
    // update_settings: merges `patch` (an object) and drops `remove`; returns the committed
    // mapping.
    Json update(const Json& patch, std::span<const std::string_view> remove = {});
    Json update(const Json& patch, std::initializer_list<std::string_view> remove);
    // mutate_settings: `fn` edits the mapping under the lock, then it is written, even when
    // nothing changed. A throwing `fn` writes nothing. Returns the committed mapping.
    Json mutate(const std::function<void(Json&)>&); // throws -> nothing written
    // save_setting: update({key: value}), logged as Python logs it.
    void save_setting(std::string_view key, const Json& value);

    // After a save through this store changes one of `keys` (Python ==, so 1 == 1.0 == true),
    // `fn` gets a JSON array of the changed keys among those watched, in watch order: inline
    // when `executor` is current on the saving thread, otherwise posted to it. A throwing
    // callback is logged "A settings watcher failed". Only saves through this store are heard.
    Subscription watch(std::vector<std::string_view> keys, Executor&,
                       std::function<void(const Json& changed)>);
    // Whether this launch started without the settings file (decided at construction).
    bool is_new_install() const;
    const std::filesystem::path& path() const;

protected:
    SettingsFile& backing_file() const { return *file_; }

private:
    struct Watcher;
    struct WatchList;
    using Before = std::vector<std::pair<std::string, std::optional<Json>>>;

    Json load_unlocked(bool strict) const;
    std::shared_ptr<const Json> read_unlocked() const;
    void write_unlocked(const Json& settings);
    Before watched_values(const Json& settings) const;
    void tell_watchers(const Before& before, const Json& settings) const;

    std::unique_ptr<SettingsFile> file_;
    bool existed_at_start_ = false;
    mutable std::recursive_mutex mutex_;
    mutable std::optional<FileSignature> cached_signature_;
    mutable std::shared_ptr<const Json> cached_;
    std::shared_ptr<WatchList> watchers_;
};

} // namespace fv
