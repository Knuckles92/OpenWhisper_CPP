#pragma once

#include <functional>

#include <QString>
#include <QUrl>

namespace fv {

// Shows path in the file manager, selected where the platform allows (Windows: Explorer's
// /select; elsewhere, and on any failure, the folder holding it). Revealing a file is cosmetic,
// so callers get a bool and never an exception.
bool revealInFileManager(const QString& path);

// Opens path itself so its contents are visible. Use this instead of revealInFileManager when
// the folder is the thing the user wants: selecting it would only highlight it inside its
// parent. A folder that is gone is reported, never quietly swapped for its parent.
bool openFolderInFileManager(const QString& path);

// What the two functions launch with. Both default to the real thing; a test replaces them to
// record what would have been launched.
struct FileRevealHooks {
    // QDesktopServices::openUrl.
    std::function<bool(const QUrl&)> openUrl;
    // Starts a detached process from a raw Windows command line, as `explorer /select,"<path>"`
    // (Explorer only honours the selection when the path is quoted after "/select,", which an
    // argument list cannot produce); returns whether it started. Its exit code is never read:
    // Explorer exits 1 even on success.
    std::function<bool(const QString& commandLine)> startDetached;
};

// Replaces the hooks (an empty member keeps the real behaviour); returns the previous ones.
FileRevealHooks setFileRevealHooks(FileRevealHooks hooks);

} // namespace fv
