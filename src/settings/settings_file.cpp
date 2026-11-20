#include "settings/settings_file.h"

#include <system_error>
#include <utility>

#include "base/atomic_file.h"
#include "settings/detail/disk_file.h"

namespace fv {

namespace {

class DiskSettingsFile final : public SettingsFile {
public:
    explicit DiskSettingsFile(std::filesystem::path path) : path_(std::move(path)) {}

    const std::filesystem::path& path() const override { return path_; }

    std::optional<FileSignature> signature() const override {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path_, error);
        if (error) {
            return std::nullopt;
        }
        return detail::stat_signature(absolute.lexically_normal());
    }

    bool exists() const override {
        std::error_code error;
        return std::filesystem::exists(path_, error) && !error;
    }

    std::string read() const override { return detail::read_whole_file(path_); }

    void replace(std::string_view bytes) override { write_file_atomic(path_, bytes); }

private:
    std::filesystem::path path_;
};

} // namespace

std::unique_ptr<SettingsFile> disk_settings_file(std::filesystem::path path) {
    return std::make_unique<DiskSettingsFile>(std::move(path));
}

} // namespace fv
