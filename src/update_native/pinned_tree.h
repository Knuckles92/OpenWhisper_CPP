#pragma once
#include "base/json.h"
#include "update_native/guarded_directory.h"
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace fv::update_native {
// Holds all observed directories and files stable while authenticating and launching. The caller
// must release this object before renaming the tree, then reconstruct and authenticate it again.
class PinnedTree {
public:
    explicit PinnedTree(const std::filesystem::path& root);
    void verify_release(std::string_view version, std::string_view executable_sha256,
                        const Json& compatibility, bool marker_required = true) const;
    void verify_old(std::string_view executable_sha256) const;

private:
    std::filesystem::path root_;
    std::vector<std::unique_ptr<GuardedDirectory>> directories_;
    std::vector<std::unique_ptr<GuardedDirectory::FilePin>> files_;
    std::map<std::string, std::filesystem::path> observed_;
    std::set<std::string> observed_directories_;
};
} // namespace fv::update_native
