#include "automation/detail/pypath.h"

#include <algorithm>

#include "pyport/text.h"

namespace fv::automation::detail {

namespace {

#ifdef _WIN32
std::string with_backslashes(std::string_view text) {
    std::string out(text);
    std::ranges::replace(out, '/', '\\');
    return out;
}

// The part of ntpath.splitroot(p) after the drive and root: (drive, root, rest)[2].
std::string_view after_root(std::string_view p) {
    const std::string normp = with_backslashes(p);
    if (normp.starts_with('\\')) {
        if (normp.size() > 1 && normp[1] == '\\') {
            // UNC and device drives: \\server\share, \\?\UNC\server\share, \\.\device.
            std::string head = py::upper(normp.substr(0, 8));
            const std::size_t start = head == "\\\\?\\UNC\\" ? 8 : 2;
            const auto index = normp.find('\\', start);
            if (index == std::string::npos) {
                return {};
            }
            const auto index2 = normp.find('\\', index + 1);
            if (index2 == std::string::npos) {
                return {};
            }
            return p.substr(index2 + 1);
        }
        return p.substr(1);
    }
    // normp[1:2] == ":" counts code points: the drive is the first one and the colon.
    const std::string first = py::slice(normp, 0, 1);
    if (normp.size() > first.size() && normp[first.size()] == ':') {
        const std::size_t drive = first.size() + 1;
        if (normp.size() > drive && normp[drive] == '\\') {
            return p.substr(drive + 1);
        }
        return p.substr(drive);
    }
    return p;
}
#endif

} // namespace

bool isabs(std::string_view path) {
#ifdef _WIN32
    const std::string head = with_backslashes(py::slice(path, 0, 3));
    return head.starts_with('\\') || py::slice(head, 1).starts_with(":\\");
#else
    return path.starts_with('/');
#endif
}

std::string basename(std::string_view path) {
#ifdef _WIN32
    const std::string_view rest = after_root(path);
    const auto last = rest.find_last_of("\\/");
    return std::string(last == std::string_view::npos ? rest : rest.substr(last + 1));
#else
    const auto last = path.rfind('/');
    return std::string(last == std::string_view::npos ? path : path.substr(last + 1));
#endif
}

} // namespace fv::automation::detail
