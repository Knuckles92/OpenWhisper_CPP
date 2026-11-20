#pragma once

#include <string>
#include <string_view>

// os.path of the platform the app runs on: ntpath on Windows, posixpath elsewhere.
namespace fv::automation::detail {

// os.path.isabs. On Windows (ntpath, Python 3.12): a path whose first three characters, with
// '/' read as '\', start with '\' or have ":\" after the first, so "\x" counts and "C:x" not.
bool isabs(std::string_view path);

// os.path.basename: what follows the last separator after the drive ("C:x" gives "x", a bare
// UNC share gives "").
std::string basename(std::string_view path);

} // namespace fv::automation::detail
