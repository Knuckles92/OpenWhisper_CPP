#include "settings/testing/memory_store.h"

#include <stdexcept>
#include <utility>

#include "settings/detail/file_text.h"

namespace fv {

using detail::settings_text;

MemorySettingsFile::MemorySettingsFile(std::optional<std::string> bytes)
    : path_("memory/fiorivoce_settings.json"), bytes_(std::move(bytes)) {}

const std::filesystem::path& MemorySettingsFile::path() const {
    return path_;
}

std::optional<FileSignature> MemorySettingsFile::signature() const {
    std::lock_guard lock(mutex_);
    FileSignature signature{.path = path_};
    if (bytes_) {
        signature.exists = true;
        signature.index = generation_;
        signature.size = bytes_->size();
    }
    return signature;
}

bool MemorySettingsFile::exists() const {
    std::lock_guard lock(mutex_);
    return bytes_.has_value();
}

std::string MemorySettingsFile::read() const {
    std::lock_guard lock(mutex_);
    ++reads_;
    if (!bytes_) {
        throw SettingsFileError("FileNotFoundError", "[Errno 2] No such file or directory: '" +
                                                         path_.generic_string() + "'");
    }
    return *bytes_;
}

void MemorySettingsFile::replace(std::string_view bytes) {
    std::lock_guard lock(mutex_);
    if (failure_) {
        const std::string message = std::move(*failure_);
        failure_.reset();
        throw std::runtime_error(message);
    }
    bytes_ = std::string(bytes);
    ++generation_;
    ++writes_;
}

std::optional<std::string> MemorySettingsFile::bytes() const {
    std::lock_guard lock(mutex_);
    return bytes_;
}

void MemorySettingsFile::set_bytes(std::optional<std::string> bytes) {
    std::lock_guard lock(mutex_);
    bytes_ = std::move(bytes);
    ++generation_;
}

void MemorySettingsFile::fail_next_replace(std::string message) {
    std::lock_guard lock(mutex_);
    failure_ = std::move(message);
}

std::size_t MemorySettingsFile::reads() const {
    std::lock_guard lock(mutex_);
    return reads_;
}

std::size_t MemorySettingsFile::writes() const {
    std::lock_guard lock(mutex_);
    return writes_;
}

MemorySettingsStore::MemorySettingsStore() : MemorySettingsStore(Json::object()) {}

MemorySettingsStore::MemorySettingsStore(const Json& initial)
    : SettingsStore(std::make_unique<MemorySettingsFile>(settings_text(initial))) {}

MemorySettingsStore::MemorySettingsStore(std::nullopt_t)
    : SettingsStore(std::make_unique<MemorySettingsFile>(std::nullopt)) {}

Json MemorySettingsStore::contents() const {
    const auto bytes = file().bytes();
    return bytes ? detail::parse_settings(*bytes) : Json::object();
}

void MemorySettingsStore::set_contents(const Json& settings) {
    file().set_bytes(settings_text(settings));
}

} // namespace fv
