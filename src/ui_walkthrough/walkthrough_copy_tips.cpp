#include "ui_walkthrough/walkthrough_copy_tips.h"

#include <array>

#include <QAction>
#include <QWidget>

namespace fv {
namespace {

struct Tip {
    std::string_view format;
    const char16_t* text;
};

constexpr std::array<Tip, 5> kCopyTips{{
    {"pdf", u"Copies the PDF file to paste or attach to your chat."},
    {"agent_task", u"Copies instructions asking your agent to carry out the work in this "
                   u"walkthrough. Paste them, then attach the PDF to your chat."},
    {"both", u"Copies the PDF file and Markdown text together. The app you paste into chooses "
             u"which format to use."},
    {"agent", u"Copies Markdown text with a local file link to each screenshot, for pasting into "
              u"a terminal agent."},
    {"word", u"Copies a Word (.docx) file you can edit."},
}};

constexpr std::array<const char16_t*, 2> kAgentOnFinish{
    u"Auto-paste on: pastes the PDF, then instructions asking your agent to carry out the work "
    u"in it. Leaves the message unsent.",
    u"Auto-paste off: copies the instructions. Attach the PDF separately, or use Paste latest "
    u"walkthrough.",
};

// Python's html.escape(text), quotes included.
QString escape(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':
            out += QStringLiteral("&amp;");
            break;
        case u'<':
            out += QStringLiteral("&lt;");
            break;
        case u'>':
            out += QStringLiteral("&gt;");
            break;
        case u'"':
            out += QStringLiteral("&quot;");
            break;
        case u'\'':
            out += QStringLiteral("&#x27;");
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace

QStringList copyTipParagraphs(std::string_view format, bool onFinish) {
    if (onFinish && format == "agent_task") {
        return {QString::fromUtf16(kAgentOnFinish[0]), QString::fromUtf16(kAgentOnFinish[1])};
    }
    for (const Tip& tip : kCopyTips) {
        if (tip.format == format) {
            return {QString::fromUtf16(tip.text)};
        }
    }
    return {};
}

QString copyTipHtml(std::string_view format, bool onFinish) {
    QString body;
    for (const QString& text : copyTipParagraphs(format, onFinish)) {
        body += QStringLiteral("<p>") + escape(text) + QStringLiteral("</p>");
    }
    return QStringLiteral("<qt><table width=\"300\"><tr><td>") + body +
           QStringLiteral("</td></tr></table></qt>");
}

void setCopyTip(QWidget* target, std::string_view format, bool onFinish) {
    target->setToolTip(copyTipHtml(format, onFinish));
    target->setAccessibleDescription(copyTipParagraphs(format, onFinish).join(u"\n\n"));
}

void setCopyTip(QAction* target, std::string_view format, bool onFinish) {
    target->setToolTip(copyTipHtml(format, onFinish));
}

} // namespace fv
