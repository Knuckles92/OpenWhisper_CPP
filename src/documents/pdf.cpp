#include "documents/pdf.h"

#include <QImage>
#include <QPdfWriter>
#include <QString>

#include <cmath>

#include "base/identity.h"
#include "base/utf8.h"
#include "documents/composer.h"
#include "documents/detail/text.h"
#include "documents/markdown_composer.h"
#include "walkthrough/storage.h"

namespace fv::documents {

namespace {

void prepare(QPdfWriter& writer, const std::optional<QPageSize>& pageSize, const QString& title) {
    writer.setPageSize(pageSize.value_or(localPageSize()));
    writer.setPageMargins(kMarginsIn, QPageLayout::Inch);
    writer.setTitle(title);
    writer.setCreator(detail::qtText(identity().product));
}

} // namespace

WrittenPdf writePdf(const walkthrough::Walkthrough& walkthrough,
                    const std::filesystem::path& folder, const walkthrough::DocumentFormat& format,
                    const std::optional<QPageSize>& pageSize) {
    const std::filesystem::path path = folder / path_from_utf8(walkthrough::PDF_NAME);
    QPdfWriter writer(detail::qtPath(path));
    const std::string title = format.title.empty() ? std::string(walkthrough::TITLE) : format.title;
    prepare(writer, pageSize,
            detail::qtText(title + " · " +
                           walkthrough::when_text(walkthrough.started_at.get_local_time())));
    const int dpi = layoutDpi();
    const ComposedDocument composed =
        Composer(textArea(writer.pageLayout(), dpi), dpi, format).compose(walkthrough);
    return {path, paintPages(*composed.doc, composed.frames, writer, dpi, path)};
}

WrittenPdf writeMarkdownPdf(std::string_view text, const std::filesystem::path& path,
                            const std::filesystem::path& folder, std::string_view title,
                            const std::optional<QPageSize>& pageSize,
                            const walkthrough::Look& look) {
    QPdfWriter writer(detail::qtPath(path));
    prepare(writer, pageSize, detail::qtText(title));
    const int dpi = layoutDpi();
    const ComposedDocument composed =
        MarkdownComposer(textArea(writer.pageLayout(), dpi), dpi, folder, look).compose(text);
    return {path, paintPages(*composed.doc, composed.frames, writer, dpi, path)};
}

QPageLayout pageLayout(const std::optional<QPageSize>& pageSize) {
    return QPageLayout(pageSize.value_or(localPageSize()), QPageLayout::Portrait, kMarginsIn,
                       QPageLayout::Inch);
}

QPageSize localPageSize() {
    return pageSizeFor(QLocale::system().measurementSystem());
}

QPageSize pageSizeFor(QLocale::MeasurementSystem measurement) {
    if (measurement == QLocale::ImperialUSSystem) {
        return QPageSize(QPageSize::Letter);
    }
    return QPageSize(QPageSize::A4);
}

std::optional<QPageSize> pageSizeFor(walkthrough::PaperSize paper) {
    switch (paper) {
    case walkthrough::PaperSize::Letter:
        return QPageSize(QPageSize::Letter);
    case walkthrough::PaperSize::A4:
        return QPageSize(QPageSize::A4);
    case walkthrough::PaperSize::Auto:
        break;
    }
    return std::nullopt;
}

int layoutDpi() {
    return QImage(1, 1, QImage::Format_RGB32).logicalDpiY();
}

QSizeF textArea(const QPageLayout& layout, int dpi) {
    const QRectF paint = layout.paintRect(QPageLayout::Inch);
    return {std::floor(paint.width() * dpi), std::floor(paint.height() * dpi)};
}

} // namespace fv::documents
