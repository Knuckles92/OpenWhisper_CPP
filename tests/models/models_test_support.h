#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "models/catalog.h"
#include "models/gpu_probe.h"

namespace fv::models::test {

namespace fs = std::filesystem;

// A fresh folder under the system temp folder, removed with everything in it at the end.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        std::random_device seed;
        path_ = fs::temp_directory_path() /
                ("fv-models-test-" + std::to_string(seed()) + "-" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline void write_file(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// _make_installed: a complete component tree with a 1 KiB bin/fake.dll, the manifest and the
// sentinel (the manifest's version, or "").
inline fs::path make_installed(const fs::path& root, std::string_view component_id,
                               const Json& manifest) {
    const fs::path target = root / component_id;
    write_file(target / "bin" / "fake.dll", std::string(1024, 'x'));
    write_file(target / "manifest.json", manifest.dump());
    const auto version = manifest.find("version");
    write_file(target / ".installed", version != manifest.end() && version->is_string()
                                          ? version->get<std::string>()
                                          : std::string());
    return target;
}

// A host with a platform, a GPU (or none) and nothing installed.
inline Host host_for(std::optional<std::string> platform,
                     std::optional<NvidiaGpu> gpu = std::nullopt) {
    Host host;
    host.platform = std::move(platform);
    if (gpu) {
        host.gpu = [gpu] {
            return gpu;
        };
    }
    return host;
}

// A zip of `files` (name to bytes) with every member stored, as a release archive is read.
inline std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    const auto crc32 = [](std::string_view bytes) {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const unsigned char byte : bytes) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        return ~crc;
    };
    const auto put = [](std::string& out, std::uint32_t value, int width) {
        for (int i = 0; i < width; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
        }
    };
    // 1980-01-01 in DOS date form; the time is midnight.
    constexpr std::uint32_t kDosDate = (1u << 5) | 1u;
    std::string body;
    std::string directory;
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<std::uint32_t>(body.size());
        const std::uint32_t crc = crc32(data);
        const auto size = static_cast<std::uint32_t>(data.size());
        const auto name_size = static_cast<std::uint32_t>(name.size());
        put(body, 0x04034b50u, 4);
        put(body, 20, 2);
        put(body, 0, 2);
        put(body, 0, 2);
        put(body, 0, 2);
        put(body, kDosDate, 2);
        put(body, crc, 4);
        put(body, size, 4);
        put(body, size, 4);
        put(body, name_size, 2);
        put(body, 0, 2);
        body += name;
        body += data;
        put(directory, 0x02014b50u, 4);
        put(directory, 20, 2);
        put(directory, 20, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, kDosDate, 2);
        put(directory, crc, 4);
        put(directory, size, 4);
        put(directory, size, 4);
        put(directory, name_size, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 4);
        put(directory, offset, 4);
        directory += name;
    }
    std::string zip = body + directory;
    put(zip, 0x06054b50u, 4);
    put(zip, 0, 2);
    put(zip, 0, 2);
    put(zip, static_cast<std::uint32_t>(files.size()), 2);
    put(zip, static_cast<std::uint32_t>(files.size()), 2);
    put(zip, static_cast<std::uint32_t>(directory.size()), 4);
    put(zip, static_cast<std::uint32_t>(body.size()), 4);
    put(zip, 0, 2);
    return zip;
}

inline NvidiaGpu gpu(std::string name, std::int64_t mib, std::optional<std::pair<int, int>> cc) {
    NvidiaGpu card;
    card.name = std::move(name);
    card.total_mib = mib;
    card.compute_capability = cc;
    return card;
}

} // namespace fv::models::test
