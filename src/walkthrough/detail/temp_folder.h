#pragma once

#include <filesystem>
#include <string>

namespace fv::walkthrough::detail {

// tempfile.TemporaryDirectory(prefix=..., dir=...): a new folder named `prefix` and eight random
// characters, in `dir` (the system's temporary folder when empty), removed with what it holds
// when the object goes. Throws std::filesystem::filesystem_error when it cannot be created.
// Affinity: one thread.
class TemporaryFolder {
public:
    explicit TemporaryFolder(const std::string& prefix, const std::filesystem::path& dir = {});
    ~TemporaryFolder();
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Whether `path` holds a whole PNG: the signature, an IHDR with a size, at least one IDAT and
// the IEND chunk, each chunk inside the file with a matching CRC. The image data itself is not
// inflated (this library reads no images); a file cut short by a crash fails here.
bool readable_png(const std::filesystem::path& path);

} // namespace fv::walkthrough::detail
