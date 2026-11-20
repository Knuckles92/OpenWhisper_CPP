// Golden vectors of services.walkthrough.document_pdf: the walkthrough and Markdown composers and
// their pagination, replayed on the same fonts and DPI (tools/parity/cases/
// services.walkthrough.document_pdf.py says what each field is). Floats compare within kSlack.
#include <catch2/catch_test_macros.hpp>

#include <QPageSize>
#include <QSizeF>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextLine>

#include <fstream>

#include "base/json.h"
#include "documents/composer.h"
#include "documents/markdown_composer.h"
#include "documents/painting.h"
#include "documents/pdf.h"
#include "documents_support.h"
#include "golden/golden.h"
#include "pyport/datetime.h"

namespace fv::documents::test {
namespace {

constexpr std::string_view kModule = "services.walkthrough.document_pdf";
constexpr int kDpi = 96;

QSizeF pageOf(const Json& paper) {
    if (paper.is_array()) {
        return {paper.at(0).get<double>(), paper.at(1).get<double>()};
    }
    const std::string name = paper.get<std::string>();
    const QPageSize size(name == "letter" ? QPageSize::Letter : QPageSize::A4);
    REQUIRE((name == "letter" || name == "a4"));
    return textArea(pageLayout(size), kDpi);
}

std::chrono::local_seconds localFromIso(const std::string& iso) {
    const auto value = py::fromisoformat(iso);
    return local(value.year, static_cast<unsigned>(value.month), static_cast<unsigned>(value.day),
                 value.hour, value.minute, value.second);
}

Json describe(const QTextDocument& doc, const std::vector<Group>& groups,
              const std::vector<QRectF>& frames) {
    const double height = doc.pageSize().height();
    Json starts = Json::array();
    for (int page = 0; page < doc.pageCount(); ++page) {
        starts.push_back(nullptr);
    }
    Json images = Json::array();
    Json markers = Json::array();
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const QTextLayout* layout = block.layout();
        if (layout->lineCount()) {
            const int page = pageIndex(origin(doc, block).y() + layout->lineAt(0).y(), height);
            if (page >= 0 && page < static_cast<int>(starts.size()) &&
                starts[static_cast<std::size_t>(page)].is_null()) {
                starts[static_cast<std::size_t>(page)] = block.blockNumber();
            }
        }
        if (block.textList() != nullptr) {
            const QString text = block.text();
            const qsizetype tab = text.indexOf(QChar(u'\t'));
            markers.push_back(utf8(tab < 0 ? text : text.left(tab)));
        }
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextCharFormat format = iterator.fragment().charFormat();
            if (format.isImageFormat()) {
                const QTextImageFormat image = format.toImageFormat();
                images.push_back(Json::array({image.width(), image.height()}));
            }
        }
    }
    Json groupList = Json::array();
    for (const Group& group : groups) {
        groupList.push_back(Json::array(
            {group.first, group.image ? Json(*group.image) : Json(nullptr), group.last}));
    }
    Json frameList = Json::array();
    for (const QRectF& frame : frames) {
        frameList.push_back(Json::array({frame.x(), frame.y(), frame.width(), frame.height()}));
    }
    return Json{{"pages", doc.pageCount()}, {"page_starts", starts}, {"groups", groupList},
                {"frames", frameList},      {"images", images},      {"markers", markers}};
}

void expectMatches(const golden::Case& c, const Json& actual) {
    const Json expected = c.result<Json>();
    INFO(c.label());
    INFO("expected " << expected.dump());
    INFO("actual   " << actual.dump());
    CHECK(golden::matches(expected, actual, c.float_tolerance()));
}

TEST_CASE("the walkthrough composer lays out and paginates as OpenWhisper's",
          "[golden][test_walkthrough_document]") {
    golden::for_each(kModule, "compose", [](const golden::Case& c) {
        TempFolder tmp;
        const Json value = c.arg<Json>(1);
        walkthrough::Walkthrough walkthrough{
            walkthrough::local_moment(std::chrono::locate_zone("America/Los_Angeles"),
                                      localFromIso(value.at("started_at").get<std::string>())),
            value.at("duration_s").get<double>(),
            {}};
        int number = 0;
        for (const Json& item : value.at("sections")) {
            ++number;
            const Json& shot = item.at(1);
            std::optional<std::filesystem::path> path;
            const auto name = tmp / ("shot-" + std::to_string(number) + ".png");
            if (shot.is_string()) {
                REQUIRE(shot.get<std::string>() == "unreadable");
                std::ofstream(name, std::ios::binary) << "not a png";
                path = name;
            } else if (shot.is_array()) {
                path = png(name, shot.at(0).get<int>(), shot.at(1).get<int>());
            }
            const Json& kept = item.at(2);
            walkthrough.sections.push_back(
                section(item.at(0).get<std::string>(), path,
                        kept.is_null() ? std::nullopt : std::optional(kept.get<double>())));
        }
        walkthrough::DocumentFormat format;
        if (const Json chosen = c.arg<Json>(2); !chosen.is_null()) {
            format = {chosen.at(0).get<std::string>(), chosen.at(1).get<bool>(),
                      chosen.at(2).get<bool>(), chosen.at(3).get<bool>()};
        }

        const ComposedDocument composed =
            Composer(pageOf(c.arg<Json>(0)), kDpi, format).compose(walkthrough);

        expectMatches(c, describe(*composed.doc, composed.groups, composed.frames));
    });
}

TEST_CASE("the markdown composer lays out and paginates as OpenWhisper's",
          "[golden][test_walkthrough_write_up]") {
    golden::for_each(kModule, "compose_markdown", [](const golden::Case& c) {
        TempFolder tmp;
        for (const Json& image : c.arg<Json>(2)) {
            png(tmp / image.at(0).get<std::string>(), image.at(1).get<int>(),
                image.at(2).get<int>());
        }
        walkthrough::Look look;
        if (const Json chosen = c.arg<Json>(3); !chosen.is_null()) {
            look = {chosen.at(0).get<std::string>(), chosen.at(1).get<std::string>(),
                    chosen.at(2).get<std::string>()};
        }

        const ComposedDocument composed =
            MarkdownComposer(pageOf(c.arg<Json>(0)), kDpi, tmp.path(), look)
                .compose(c.arg<std::string>(1));

        expectMatches(c, describe(*composed.doc, composed.groups, composed.frames));
    });
}

} // namespace
} // namespace fv::documents::test
