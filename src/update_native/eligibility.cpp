#include "update_native/desktop.h"

#include <string>
#include <system_error>

#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::update_native {
namespace {
// canonical_path: os.path.normcase(os.path.realpath(path)).
std::string canonical_text(const std::filesystem::path& path) {
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error)
        resolved = path.lexically_normal();
    auto text = path_to_utf8(resolved.make_preferred());
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\'))
        text.pop_back();
#ifdef _WIN32
    text = py::lower(text);
#endif
    return text;
}
} // namespace

bool under_program_files(const std::filesystem::path& folder,
                         std::span<const std::filesystem::path> roots) {
    const auto target = canonical_text(folder);
    for (const auto& root : roots) {
        if (root.empty())
            continue;
        const auto canonical = canonical_text(root);
        if (target == canonical ||
            (target.size() > canonical.size() && target.starts_with(canonical) &&
             (target[canonical.size()] == '/' || target[canonical.size()] == '\\')))
            return true;
    }
    return false;
}

bool native_update_eligible(const NativeEligibility& facts) {
    return facts.standard_user &&
           update::detect_channel(facts.probe) == update::InstallChannel::Installer &&
           !facts.probe.all_users && !facts.under_program_files && facts.default_data_root &&
           facts.registered_program && facts.inventory_present;
}
} // namespace fv::update_native
