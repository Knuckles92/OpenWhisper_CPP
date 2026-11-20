#include "settings/store.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "base/log.h"
#include "pyport/json.h"
#include "settings/detail/file_text.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.settings"};

// The exception's message, as Python's f"{e}" shows it.
std::string message_of(const std::exception& error) {
    return error.what();
}

} // namespace

struct SettingsStore::Watcher {
    std::vector<std::string> keys; // in watch order, each once
    Executor* executor;
    std::function<void(const Json&)> fn;
    std::shared_ptr<CallbackGuard> guard;
};

struct SettingsStore::WatchList {
    std::mutex mutex;
    std::vector<std::shared_ptr<Watcher>> items;
};

SettingsStore::SettingsStore(std::filesystem::path file)
    : SettingsStore(disk_settings_file(std::move(file))) {}

SettingsStore::SettingsStore(std::unique_ptr<SettingsFile> file)
    : file_(std::move(file)), watchers_(std::make_shared<WatchList>()) {
    if (!file_) {
        throw std::invalid_argument("SettingsStore needs a settings file");
    }
    existed_at_start_ = file_->exists();
}

SettingsStore::~SettingsStore() = default;

const std::filesystem::path& SettingsStore::path() const {
    return file_->path();
}

bool SettingsStore::is_new_install() const {
    return !existed_at_start_;
}

std::optional<FileSignature> SettingsStore::file_signature() const {
    std::lock_guard lock(mutex_);
    return file_->signature();
}

Json SettingsStore::load_unlocked(bool strict) const {
    if (!file_->exists()) {
        return Json::object();
    }
    // Deliberately broad, as OpenWhisper's: every reader relies on a tolerant load never
    // throwing, whatever is wrong with the file.
    try {
        return detail::parse_settings(file_->read());
    } catch (const SettingsFileError& error) {
        kLog.warning("Failed to load all settings: {}", message_of(error));
        if (strict) {
            throw;
        }
    } catch (const std::exception& error) {
        kLog.warning("Failed to load all settings: {}", message_of(error));
        if (strict) {
            throw SettingsFileError("OSError", error.what());
        }
    }
    return Json::object();
}

std::shared_ptr<const Json> SettingsStore::read_unlocked() const {
    const auto signature = file_->signature();
    if (signature && cached_ && cached_signature_ == signature) {
        return cached_;
    }
    auto settings = std::make_shared<const Json>(load_unlocked(false));
    // A file that changed while it was read is not labelled with the earlier signature.
    if (signature && file_->signature() == signature) {
        cached_signature_ = signature;
        cached_ = settings;
    } else {
        cached_signature_.reset();
        cached_.reset();
    }
    return settings;
}

void SettingsStore::write_unlocked(const Json& settings) {
    if (!settings.is_object()) {
        throw std::invalid_argument("settings must be a mapping");
    }
    file_->replace(detail::settings_text(settings));
    // The next read owns its snapshot: never label this mapping with a signature that a
    // concurrent outside writer may already have moved on from.
    cached_signature_.reset();
    cached_.reset();
}

std::shared_ptr<const Json> SettingsStore::snapshot() const {
    std::lock_guard lock(mutex_);
    return read_unlocked();
}

Json SettingsStore::load_settings(std::span<const std::string_view> keys) const {
    const auto settings = snapshot();
    Json subset = Json::object();
    for (const std::string_view key : keys) {
        const auto found = settings->find(key);
        if (found != settings->end()) {
            subset[std::string(key)] = *found;
        }
    }
    return subset;
}

Json SettingsStore::load_settings(std::initializer_list<std::string_view> keys) const {
    return load_settings(std::span<const std::string_view>(keys.begin(), keys.size()));
}

Json SettingsStore::value(std::string_view key, const Json& fallback) const {
    const auto settings = snapshot();
    const auto found = settings->find(key);
    return found == settings->end() ? fallback : *found;
}

SettingsStore::Before SettingsStore::watched_values(const Json& settings) const {
    Before before;
    std::lock_guard lock(watchers_->mutex);
    for (const auto& watcher : watchers_->items) {
        for (const std::string& key : watcher->keys) {
            if (std::ranges::any_of(before,
                                    [&](const auto& entry) { return entry.first == key; })) {
                continue;
            }
            const auto found = settings.find(key);
            before.emplace_back(key, found == settings.end() ? std::nullopt
                                                             : std::optional<Json>(*found));
        }
    }
    return before;
}

void SettingsStore::tell_watchers(const Before& before, const Json& settings) const {
    std::vector<std::string_view> changed;
    for (const auto& [key, value] : before) {
        const auto found = settings.find(key);
        const bool now_set = found != settings.end();
        if (now_set != value.has_value() || (now_set && !py::py_equal(*found, *value))) {
            changed.push_back(key);
        }
    }
    if (changed.empty()) {
        return;
    }
    std::vector<std::shared_ptr<Watcher>> watchers;
    {
        std::lock_guard lock(watchers_->mutex);
        watchers = watchers_->items;
    }
    for (const auto& watcher : watchers) {
        Json keys = Json::array();
        for (const std::string& key : watcher->keys) {
            if (std::ranges::find(changed, key) != changed.end()) {
                keys.push_back(key);
            }
        }
        if (keys.empty()) {
            continue;
        }
        auto call = [watcher, keys = std::move(keys)] {
            watcher->guard->run([&] {
                try {
                    watcher->fn(keys);
                } catch (const std::exception& error) {
                    kLog.error_with(error, "A settings watcher failed");
                } catch (...) {
                    kLog.error("A settings watcher failed");
                }
            });
        };
        if (watcher->executor->is_current()) {
            call();
        } else {
            watcher->executor->post(std::move(call));
        }
    }
}

void SettingsStore::save_all(const Json& settings) {
    try {
        Before before;
        {
            std::lock_guard lock(mutex_);
            bool watched = false;
            {
                std::lock_guard watch_lock(watchers_->mutex);
                watched = !watchers_->items.empty();
            }
            if (watched) {
                before = watched_values(load_unlocked(false));
            }
            write_unlocked(settings);
        }
        tell_watchers(before, settings);
        kLog.info("All settings saved successfully");
    } catch (const std::exception& error) {
        kLog.error("Failed to save all settings: {}", message_of(error));
        throw;
    }
}

Json SettingsStore::mutate(const std::function<void(Json&)>& fn) {
    if (!fn) {
        throw std::invalid_argument("mutator must be callable");
    }
    Before before;
    Json settings;
    {
        std::lock_guard lock(mutex_);
        settings = load_unlocked(true);
        before = watched_values(settings);
        fn(settings);
        write_unlocked(settings);
    }
    tell_watchers(before, settings);
    return settings;
}

Json SettingsStore::update(const Json& patch, std::span<const std::string_view> remove) {
    if (!patch.is_object()) {
        throw std::invalid_argument("updates must be a mapping");
    }
    return mutate([&](Json& settings) {
        for (const auto& [key, value] : patch.items()) {
            settings[key] = value;
        }
        for (const std::string_view key : remove) {
            const auto found = settings.find(key);
            if (found != settings.end()) {
                settings.erase(found);
            }
        }
    });
}

Json SettingsStore::update(const Json& patch, std::initializer_list<std::string_view> remove) {
    return update(patch, std::span<const std::string_view>(remove.begin(), remove.size()));
}

void SettingsStore::save_setting(std::string_view key, const Json& value) {
    try {
        Json patch = Json::object();
        patch[std::string(key)] = value;
        update(patch);
        // OpenWhisper also logs the value; a value may be dictated text or a prompt, which the
        // log facade never takes, so only the key is logged.
        kLog.debug("Setting saved: {}", key);
    } catch (const std::exception& error) {
        kLog.error("Failed to save setting '{}': {}", key, message_of(error));
        throw;
    }
}

Subscription SettingsStore::watch(std::vector<std::string_view> keys, Executor& executor,
                                  std::function<void(const Json& changed)> fn) {
    auto watcher = std::make_shared<Watcher>();
    for (const std::string_view key : keys) {
        if (std::ranges::find(watcher->keys, key) == watcher->keys.end()) {
            watcher->keys.emplace_back(key);
        }
    }
    watcher->executor = &executor;
    watcher->fn = std::move(fn);
    watcher->guard = std::make_shared<CallbackGuard>();
    {
        std::lock_guard lock(watchers_->mutex);
        watchers_->items.push_back(watcher);
    }
    std::weak_ptr<WatchList> list = watchers_;
    const Watcher* entry = watcher.get();
    return Subscription(watcher->guard, [list, entry] {
        if (const auto watchers = list.lock()) {
            std::lock_guard lock(watchers->mutex);
            std::erase_if(watchers->items,
                          [entry](const auto& item) { return item.get() == entry; });
        }
    });
}

} // namespace fv
