#include "clipboard/html.h"

#include <QMimeData>

#include "appkit/qt_bridge.h"
#include "pyport/text.h"

namespace fv::clipboard {

std::string html_document(std::string_view html) {
    if (py::lower(py::slice(py::lstrip(html), 0, 5)) == "<html") {
        return std::string(html);
    }
    std::string document = R"(<html><head><meta charset="utf-8"></head><body><!--StartFragment-->)";
    document += html;
    document += "<!--EndFragment--></body></html>";
    return document;
}

std::unique_ptr<QMimeData> text_mime_data(const QString& text, const QString& html) {
    auto mime_data = std::make_unique<QMimeData>();
    mime_data->setText(text);
    if (!html.isEmpty()) {
        mime_data->setHtml(qt::toQString(html_document(qt::toUtf8(html))));
    }
    return mime_data;
}

} // namespace fv::clipboard
