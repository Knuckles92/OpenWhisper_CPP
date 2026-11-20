#include "ui_walkthrough/past_walkthroughs.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#include <utility>

#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStringDecoder>
#include <QVBoxLayout>

#include "base/executor.h"
#include "base/json.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_walkthrough/detail/mailbox.h"
#include "ui_walkthrough/review_text.h"
#include "walkthrough/document.h"
#include "walkthrough/files.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"
#include "walkthrough/write_up_names.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.past_walkthroughs"};

namespace review = walkthrough_review;

QString qs(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString qpath(const std::filesystem::path& path) {
    return qs(path_to_utf8(path));
}

std::filesystem::path named(const std::filesystem::path& folder, std::string_view name) {
    return folder / path_from_utf8(name);
}

// Path.read_text(encoding="utf-8"): strict UTF-8 with its BOM kept, universal newlines; nullopt
// where Python raised OSError or UnicodeDecodeError.
std::optional<QString> readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return std::nullopt;
    }
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::ConvertInitialBom);
    QString text =
        decoder.decode(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
    if (decoder.hasError()) {
        return std::nullopt;
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

// The "# heading" line of a write-up (_HEADING). (*LF) keeps PCRE's ^, $ and . to Python's
// newline, which is "\n" alone.
const QRegularExpression& headingPattern() {
    static const QRegularExpression pattern(QStringLiteral("(*LF)^#\\s+(.+?)\\s*#*\\s*$"),
                                            QRegularExpression::MultilineOption |
                                                QRegularExpression::UseUnicodePropertiesOption);
    return pattern;
}

// The manifest's start as Python's datetime keeps it: an aware start keeps its own wall time,
// where the loaded document holds it in this computer's zone. nullopt when it cannot be read.
std::optional<std::chrono::local_seconds> manifestStart(const std::filesystem::path& folder) {
    try {
        const Json manifest = walkthrough::read_json(named(folder, walkthrough::MANIFEST_NAME));
        const auto when = py::fromisoformat(manifest.at("started_at").get<std::string>());
        const std::chrono::year_month_day date{
            std::chrono::year{when.year}, std::chrono::month{static_cast<unsigned>(when.month)},
            std::chrono::day{static_cast<unsigned>(when.day)}};
        return std::chrono::local_days{date} + std::chrono::hours{when.hour} +
               std::chrono::minutes{when.minute} + std::chrono::seconds{when.second};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string folderName(const std::filesystem::path& folder) {
    return path_to_utf8(folder.filename());
}

// Whether summarize's `except (OSError, ValueError, KeyError, TypeError)` takes `failure`.
bool loadFailure(const py::PyError& failure) {
    if (dynamic_cast<const py::ValueError*>(&failure) != nullptr) {
        return true;
    }
    // ValueError's subclasses that pyport raises as a plain PyError, and OSError's.
    const std::string_view type = failure.type_name();
    return type == "ValueError" || type == "UnicodeDecodeError" || type == "JSONDecodeError" ||
           type == "KeyError" || type == "TypeError" || type == "OSError" ||
           type == "FileNotFoundError" || type == "PermissionError" || type == "TimeoutError";
}

} // namespace

namespace past_walkthroughs {

std::chrono::year_month_day toDate(const QDate& date) {
    return std::chrono::year_month_day{std::chrono::year{date.year()},
                                       std::chrono::month{static_cast<unsigned>(date.month())},
                                       std::chrono::day{static_cast<unsigned>(date.day())}};
}

std::string title(const std::filesystem::path& folder, const walkthrough::Walkthrough& document) {
    if (const auto text = readText(named(folder, walkthrough::write_up::MARKDOWN_NAME))) {
        const QRegularExpressionMatch heading = headingPattern().match(*text);
        if (heading.hasMatch()) {
            return heading.captured(1).toStdString();
        }
    }
    std::string said;
    for (const auto& section : document.sections) {
        if (py::strip(section.text).empty()) {
            continue;
        }
        std::string collapsed;
        for (const std::string& word : py::split(section.text)) {
            collapsed += collapsed.empty() ? word : " " + word;
        }
        said += said.empty() ? collapsed : " " + collapsed;
    }
    if (py::len(said) <= TITLE_CHARS) {
        return said.empty() ? std::string("No words") : said;
    }
    std::string cut = py::slice(said, 0, static_cast<std::int64_t>(TITLE_CHARS));
    if (const auto space = cut.rfind(' '); space != std::string::npos) {
        cut.resize(space);
    }
    return cut + "…";
}

QImage thumb(const std::filesystem::path& path, double ratio, QSize box, double radius) {
    QImageReader reader(qpath(path));
    const QSize size = reader.size();
    if (!size.isValid()) {
        return {};
    }
    const QSize target = box * ratio;
    const QSize cover = size.scaled(target, Qt::KeepAspectRatioByExpanding);
    reader.setScaledSize(cover);
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    // Centred across, from the top down: a screenshot's heading is up there.
    const int x = std::max(0, (image.width() - target.width()) / 2);
    image = image.copy(QRect(x, 0, target.width(), target.height()));
    QImage rounded(image.size(), QImage::Format_ARGB32_Premultiplied);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rounded.rect()), radius * ratio, radius * ratio);
    painter.setClipPath(clip);
    painter.drawImage(0, 0, image);
    painter.end();
    rounded.setDevicePixelRatio(ratio);
    return rounded;
}

Summary summarize(const std::filesystem::path& folder, std::chrono::year_month_day today,
                  double ratio, QSize thumbSize, double thumbRadius) {
    std::error_code error;
    const bool draft = std::filesystem::exists(named(folder, walkthrough::DRAFT_NAME), error);
    std::optional<walkthrough::Walkthrough> document;
    try {
        document = walkthrough::load_document(folder);
    } catch (const py::PyError& failure) {
        // Python's except clause takes OSError, ValueError (with its UnicodeDecodeError and
        // JSONDecodeError), KeyError and TypeError; anything else, such as the AttributeError of
        // a section that is not an object, leaves summarize and the folder gets no card.
        if (!loadFailure(failure)) {
            throw;
        }
        kLog.debug("Walkthrough {} can't be loaded: {}", path_to_utf8(folder), failure.what());
    } catch (const std::system_error& failure) {
        // OSError, filesystem_error among them.
        kLog.debug("Walkthrough {} can't be read: {}", path_to_utf8(folder), failure.what());
    }
    std::optional<std::chrono::local_seconds> when;
    if (document) {
        when = manifestStart(folder);
    }
    if (!when) {
        when = review::started(folder, document ? &*document : nullptr);
    }
    Summary summary;
    summary.folder = folder;
    summary.when = when;
    if (!document) {
        summary.title = draft ? "Not finished" : "Can't be opened";
        summary.meta = when ? review::day(*when, today) : folderName(folder);
        summary.pill = draft ? "Interrupted" : "";
        summary.pill_tone = "warning";
        summary.words = py::lower(folderName(folder));
        return summary;
    }
    std::vector<std::filesystem::path> shots;
    for (const auto& section : document->sections) {
        if (section.shot) {
            shots.push_back(*section.shot);
        }
    }
    if (!shots.empty()) {
        try {
            summary.thumb = thumb(shots.front(), ratio, thumbSize, thumbRadius);
        } catch (const std::exception& failure) {
            kLog.debug("No thumbnail for {}: {}", path_to_utf8(folder), failure.what());
        }
    }
    summary.title = title(folder, *document);
    const bool writeUp =
        std::filesystem::is_regular_file(named(folder, walkthrough::write_up::PDF_NAME), error);
    summary.meta = review::shots(static_cast<int>(shots.size())) + " · " +
                   walkthrough::clock(document->duration_s);
    summary.pill = writeUp ? "AI write-up" : "";
    std::string words = summary.title;
    for (const auto& section : document->sections) {
        words += " " + section.text;
    }
    summary.words = py::lower(words);
    return summary;
}

FilterView filter(std::span<const std::string> words, std::string_view search,
                  std::optional<int> total) {
    const std::vector<std::string> terms = py::split(py::lower(search));
    FilterView view;
    int shown = 0;
    for (const std::string& card : words) {
        const bool match = std::ranges::all_of(
            terms, [&card](const std::string& term) { return card.find(term) != card.npos; });
        view.visible.push_back(match);
        shown += match ? 1 : 0;
    }
    if (!total) {
        return view;
    }
    view.counted = true;
    const int cards = static_cast<int>(words.size());
    const std::string more = *total > cards ? "+" : "";
    view.count = std::to_string(shown) + more +
                 (shown == 1 && more.empty() ? " walkthrough" : " walkthroughs");
    view.count_shown = shown > 0;
    if (cards == 0) {
        view.message = "Nothing yet. Your walkthroughs show up here, newest first, with their "
                       "first screenshot.";
    } else if (shown == 0) {
        view.message = "No walkthrough says that";
    } else if (!more.empty() && terms.empty()) {
        view.message = "Showing the newest " + std::to_string(cards) + ". Open library for all " +
                       std::to_string(*total) + ".";
    }
    view.message_shown = !view.message.empty();
    return view;
}

} // namespace past_walkthroughs

// ---- the card ----

WalkthroughCard::WalkthroughCard(past_walkthroughs::Summary summary,
                                 std::chrono::year_month_day today, QWidget* parent)
    : QFrame(parent), summary_(std::move(summary)) {
    setObjectName(QStringLiteral("walkthroughCard"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(14, 12, 14, 12);
    column->setSpacing(6);

    auto* top = new QHBoxLayout();
    top->setSpacing(6);
    const auto& when = summary_.when;
    whenLabel_ =
        new QLabel(when ? qs(review::day(*when, today) + " · " + review::time(*when)) : QString());
    whenLabel_->setObjectName(QStringLiteral("walkthroughCardWhen"));
    top->addWidget(whenLabel_);
    top->addStretch(1);
    if (!summary_.pill.empty()) {
        auto* pill = new QLabel(qs(summary_.pill));
        pill->setObjectName(QStringLiteral("walkthroughCardPill"));
        if (!summary_.pill_tone.empty()) {
            ui::setStyleProperty(pill, ui::prop::tone, qs(summary_.pill_tone));
        }
        top->addWidget(pill);
    }
    column->addLayout(top);

    titleLabel_ = new QLabel(qs(summary_.title));
    titleLabel_->setObjectName(QStringLiteral("walkthroughCardTitle"));
    titleLabel_->setWordWrap(true);
    column->addWidget(titleLabel_);

    if (!summary_.thumb.isNull()) {
        auto* shot = new QLabel();
        shot->setObjectName(QStringLiteral("walkthroughCardShot"));
        shot->setPixmap(QPixmap::fromImage(summary_.thumb));
        shot->setFixedHeight(past_walkthroughs::THUMB_SIZE.height());
        shot->setAlignment(Qt::AlignCenter);
        shot->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        column->addWidget(shot);
    }

    auto* meta = new QLabel(qs(summary_.meta));
    meta->setObjectName(QStringLiteral("walkthroughCardMeta"));
    column->addWidget(meta);
    setAccessibleName(qs(summary_.title) + QStringLiteral(". ") + whenLabel_->text() +
                      QStringLiteral(", ") + qs(summary_.meta));
}

void WalkthroughCard::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        Q_EMIT clicked(qpath(summary_.folder));
    }
    QFrame::mouseReleaseEvent(event);
}

void WalkthroughCard::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        Q_EMIT clicked(qpath(summary_.folder));
        return;
    }
    QFrame::keyPressEvent(event);
}

// ---- the panel ----

struct PastWalkthroughsPanel::Inbox : ui_walkthrough::detail::Mailbox<PastWalkthroughsPanel> {
    using Mailbox::Mailbox;
};

namespace {

QString panelSheet() {
    QFile file(QStringLiteral(":/ui_walkthrough/past_walkthroughs.qss"));
    if (!file.open(QIODevice::ReadOnly)) {
        kLog.warning("The Walkthroughs page's stylesheet is missing from the resources");
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

PastWalkthroughsPanel::PastWalkthroughsPanel(std::filesystem::path root,
                                             std::function<QDate()> today, QWidget* parent)
    : QWidget(parent), root_(std::move(root)), today_(std::move(today)),
      inbox_(std::make_shared<Inbox>(this)) {
    setObjectName(QStringLiteral("walkthroughsContent"));
    setAttribute(Qt::WA_StyledBackground, true);
    setupUi();
    ui::setLocalStyleSheet(this, panelSheet());
}

PastWalkthroughsPanel::~PastWalkthroughsPanel() {
    inbox_->close();
}

void PastWalkthroughsPanel::setExecutor(Executor* io) {
    io_ = io;
}

void PastWalkthroughsPanel::setMaxShown(int count) {
    maxShown_ = count;
}

std::chrono::year_month_day PastWalkthroughsPanel::today() const {
    return past_walkthroughs::toDate(today_ ? today_() : QDate::currentDate());
}

void PastWalkthroughsPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    auto* heading = new QLabel(QStringLiteral("Walkthroughs"));
    heading->setObjectName(QStringLiteral("sidebarHeader"));
    heading->setFont(QFont(QStringLiteral("Segoe UI"), 16, QFont::Bold));
    head->addWidget(heading);
    head->addStretch(1);
    libraryButton_ = new QPushButton(QStringLiteral("Open library"));
    libraryButton_->setObjectName(QStringLiteral("walkthroughsLibraryLink"));
    libraryButton_->setCursor(Qt::PointingHandCursor);
    libraryButton_->setToolTip(QStringLiteral("Review, fix, copy and delete walkthroughs"));
    connect(libraryButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT walkthroughSelected(QString()); });
    head->addWidget(libraryButton_);
    layout->addLayout(head);

    searchInput_ = new QLineEdit();
    searchInput_->setObjectName(QStringLiteral("historySearchInput"));
    searchInput_->setPlaceholderText(QStringLiteral("Search walkthroughs..."));
    searchInput_->setClearButtonEnabled(true);
    searchInput_->setFixedHeight(32);
    connect(searchInput_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    layout->addWidget(searchInput_);

    scrollArea_ = new QScrollArea();
    scrollArea_->setObjectName(QStringLiteral("historyScrollArea"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget();
    body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(0, 0, 6, 0);
    column->setSpacing(12);
    column->setAlignment(Qt::AlignTop);
    // The page title already says Walkthroughs; this line only counts.
    countHeader_ = new QLabel(QString());
    countHeader_->setObjectName(QStringLiteral("sectionHeader"));
    countHeader_->hide();
    countHeader_->setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::DemiBold));
    column->addWidget(countHeader_);
    cardLayout_ = new QVBoxLayout();
    cardLayout_->setSpacing(12);
    column->addLayout(cardLayout_);
    emptyLabel_ = new QLabel(QStringLiteral("Loading walkthroughs…"));
    emptyLabel_->setObjectName(QStringLiteral("walkthroughsEmpty"));
    emptyLabel_->setWordWrap(true);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    column->addWidget(emptyLabel_);
    scrollArea_->setWidget(body);
    layout->addWidget(scrollArea_, 1);
}

void PastWalkthroughsPanel::refresh() {
    const std::uint64_t token = ++issued_;
    token_ = token;
    const std::filesystem::path root = root_;
    const auto today = this->today();
    const double ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    const int most = maxShown_;
    Task load = [inbox = inbox_, root, today, ratio, most, token] {
        std::vector<std::filesystem::path> folders;
        try {
            folders = walkthrough::entries(root);
        } catch (const std::filesystem::filesystem_error& failure) {
            kLog.debug("The captures folder can't be read: {}", failure.what());
        } catch (const std::exception& failure) {
            // Python's worker dies here and the page keeps saying it is loading.
            kLog.warning("Past walkthroughs could not be listed: {}", failure.what());
            return;
        }
        std::vector<past_walkthroughs::Summary> summaries;
        const auto shown = std::min(folders.size(), static_cast<std::size_t>(std::max(0, most)));
        for (std::size_t i = 0; i < shown; ++i) {
            try {
                summaries.push_back(past_walkthroughs::summarize(folders[i], today, ratio));
            } catch (const std::exception& failure) {
                kLog.warning("Could not read walkthrough {}: {}", path_to_utf8(folders[i]),
                             failure.what());
            }
        }
        const int total = static_cast<int>(folders.size());
        inbox->post(
            [token, summaries = std::move(summaries), total](PastWalkthroughsPanel& panel) mutable {
                panel.loaded(token, std::move(summaries), total);
            });
    };
    if (io_ != nullptr) {
        io_->post(std::move(load));
    } else {
        std::thread(std::move(load)).detach();
    }
}

void PastWalkthroughsPanel::loaded(std::uint64_t token,
                                   std::vector<past_walkthroughs::Summary> summaries, int total) {
    if (token != token_) {
        return;
    }
    const auto day = today();
    for (WalkthroughCard* card : cards_) {
        card->hide();
        card->setParent(nullptr);
        card->deleteLater();
    }
    cards_.clear();
    for (auto& summary : summaries) {
        auto* card = new WalkthroughCard(std::move(summary), day);
        connect(card, &WalkthroughCard::clicked, this, &PastWalkthroughsPanel::walkthroughSelected);
        cardLayout_->addWidget(card);
        cards_.push_back(card);
    }
    total_ = total;
    applyFilter();
}

void PastWalkthroughsPanel::applyFilter() {
    std::vector<std::string> words;
    words.reserve(cards_.size());
    for (const WalkthroughCard* card : cards_) {
        words.push_back(card->summary().words);
    }
    const auto view = past_walkthroughs::filter(words, searchInput_->text().toStdString(), total_);
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        cards_[i]->setVisible(view.visible[i]);
    }
    if (!view.counted) {
        return;
    }
    countHeader_->setText(qs(view.count));
    countHeader_->setVisible(view.count_shown);
    emptyLabel_->setText(qs(view.message));
    emptyLabel_->setVisible(view.message_shown);
}

} // namespace fv
