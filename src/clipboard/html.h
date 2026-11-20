#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <QString>

class QMimeData;

namespace fv::clipboard {

// html as a document that declares UTF-8 and marks its fragment; a whole document (one that
// starts, after whitespace, with "<html" in any case) is returned as it is. CF_HTML is UTF-8, but
// some readers (older Office) guess the ANSI code page for markup without a charset and garble
// accents and curly quotes, so the tag is written the way browsers write it. The fragment
// markers keep Qt from adding its own around the whole document.
std::string html_document(std::string_view html);

// Plain text, with html (as html_document) beside it when html is not empty.
// Affinity: any thread (the QMimeData is unparented; move it to the GUI thread's clipboard).
std::unique_ptr<QMimeData> text_mime_data(const QString& text, const QString& html = {});

} // namespace fv::clipboard
