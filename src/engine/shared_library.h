#pragma once

#include <filesystem>

namespace fv {

// A dynamically loaded library, unloaded when the object dies.
class SharedLibrary {
public:
    SharedLibrary() = default;
    // Loads `path`, finding the library's own dependencies in its folder.
    // Throws EngineError with the system's reason when it cannot load.
    explicit SharedLibrary(const std::filesystem::path& path);
    ~SharedLibrary();

    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    // The exported symbol's address, or nullptr when the library has none.
    void* symbol(const char* name) const;

    explicit operator bool() const { return handle_ != nullptr; }
    const std::filesystem::path& path() const { return path_; }

private:
    void close();

    void* handle_ = nullptr;
    std::filesystem::path path_;
};

} // namespace fv
