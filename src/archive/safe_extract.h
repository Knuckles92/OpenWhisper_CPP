#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

#include "base/cancel.h"

// Extraction that refuses archives trying to write outside their folder (the zip and native-tar
// extractors of services/components.py). Blocking: never on the GUI thread.
//
// Both check `cancel` before each member and throw ExtractCanceled; an unsafe member throws
// ArchiveError with the user's message; an unreadable archive throws BadArchive; disk failures
// throw std::system_error. Members already written stay on disk (callers extract into a staging
// folder they remove).
namespace fv::archive {

// (members done, member count), after each member.
using ExtractProgress = std::function<void(std::int64_t done, std::int64_t total)>;

// _safe_extract: every member of a zip into `target_dir`. A name that starts with "/" or has a
// ".." part is refused ("Archive contains an unsafe path: <name>"); the rest are written as
// zipfile.extract writes them (drive letters and "." parts dropped, and on Windows the
// characters :<>|"?* turned into "_" and trailing dots and spaces removed from each part).
void extract_zip(const std::filesystem::path& archive, const std::filesystem::path& target_dir,
                 const ExtractProgress& progress, const CancelToken& cancel);

// _safe_extract_native_tar: a native speech release (.tar.gz) under `folder/`. Every member must
// sit under the archive's own top folder `root` ("Archive contains an unsafe path: <name>"),
// which is renamed to `folder`, hard-link targets with it ("Archive contains an unsafe link:
// <linkname>"). Each member then passes tarfile's "data" filter: absolute names, names or link
// targets resolving outside `target_dir`, devices and FIFOs are refused ("Unsafe speech runtime
// archive: <filter message>"). Symbolic links are created as such where the system allows it,
// else (Windows without the privilege) as a copy of the member they name, as tarfile does.
void extract_native_tar(const std::filesystem::path& archive,
                        const std::filesystem::path& target_dir, std::string_view root,
                        std::string_view folder, const ExtractProgress& progress,
                        const CancelToken& cancel);

} // namespace fv::archive
