#include "ui_record/transcript_viewer_dialog.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QTabBar>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ui_theme/font_scale.h"
#include "ui_theme/markdown_render.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

QString formatSection(const dictation::BatchItem& item, const QString& text) {
    const std::pair<std::string, std::string> section{item.source_name(), text.toStdString()};
    return QString::fromStdString(dictation::format_batch_transcript(
        std::span<const std::pair<std::string, std::string>>(&section, 1)));
}

QPushButton* makeZoomButton(const QString& text, const QString& tooltip) {
    auto* button = new QPushButton(text);
    button->setObjectName(QStringLiteral("transcriptViewerZoomButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tooltip);
    button->setFixedSize(28, 28);
    return button;
}

} // namespace

ReaderView::ReaderView(QWidget* parent) : QTextBrowser(parent) {
    setReadOnly(true);
    setOpenExternalLinks(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void ReaderView::resizeEvent(QResizeEvent* event) {
    const int side = std::max(kMinSideMargin, (width() - kMaxMeasure) / 2);
    setViewportMargins(side, kTopMargin, side, kBottomMargin);
    QTextBrowser::resizeEvent(event);
}

void ReaderView::wheelEvent(QWheelEvent* event) {
    // QTextEdit's own Ctrl+wheel zoom changes the widget font, which the Markdown renderer
    // overrides on the next render; route it to the dialog's zoom instead so the two stay in
    // step.
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            Q_EMIT zoomRequested(delta > 0 ? 1 : -1);
        }
        event->accept();
        return;
    }
    QTextBrowser::wheelEvent(event);
}

TranscriptViewerDialog::TranscriptViewerDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("transcriptViewerDialog"));
    setWindowTitle(QStringLiteral("Transcript"));
    setModal(false);
    setWindowFlag(Qt::MSWindowsFixedSizeDialogHint, false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setMinimumSize(560, 440);
    resize(940, 760);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* toolbar = new QFrame;
    toolbar->setObjectName(QStringLiteral("transcriptViewerToolbar"));
    auto* bar = new QHBoxLayout(toolbar);
    bar->setContentsMargins(20, 12, 16, 12);
    bar->setSpacing(10);

    titleLabel_ = new ElidingLabel(QStringLiteral("Transcript"));
    titleLabel_->setObjectName(QStringLiteral("transcriptViewerTitle"));
    bar->addWidget(titleLabel_);

    pageTabs_ = new QTabBar;
    pageTabs_->setObjectName(QStringLiteral("transcriptViewerTabs"));
    pageTabs_->setDocumentMode(true);
    pageTabs_->setDrawBase(false);
    pageTabs_->setExpanding(false);
    pageTabs_->setUsesScrollButtons(true);
    pageTabs_->setElideMode(Qt::ElideRight);
    pageTabs_->setFocusPolicy(Qt::NoFocus);
    connect(pageTabs_, &QTabBar::currentChanged, this, [this](int index) { showPage(index); });
    pageTabs_->hide();
    bar->addWidget(pageTabs_, 1);

    toolbarSpacer_ = new QWidget;
    toolbarSpacer_->setObjectName(QStringLiteral("transcriptViewerToolbarSpacer"));
    bar->addWidget(toolbarSpacer_, 1);

    versionToggle_ = new QWidget;
    versionToggle_->setObjectName(QStringLiteral("transcriptSwitchRow"));
    auto* versionRow = new QHBoxLayout(versionToggle_);
    versionRow->setContentsMargins(0, 0, 0, 0);
    versionRow->setSpacing(0);
    auto* switchFrame = new QFrame;
    switchFrame->setObjectName(QStringLiteral("transcriptSwitch"));
    auto* switchLayout = new QHBoxLayout(switchFrame);
    switchLayout->setContentsMargins(2, 2, 2, 2);
    switchLayout->setSpacing(2);
    versionGroup_ = new QButtonGroup(this);
    fixedButton_ = new QPushButton(QStringLiteral("Fixed"));
    rawButton_ = new QPushButton(QStringLiteral("Raw"));
    for (QPushButton* button : {fixedButton_, rawButton_}) {
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setObjectName(QStringLiteral("transcriptSwitchBtn"));
        button->setFixedHeight(24);
        versionGroup_->addButton(button);
        switchLayout->addWidget(button);
    }
    versionRow->addWidget(switchFrame);
    fixedButton_->setChecked(true);
    connect(fixedButton_, &QPushButton::toggled, this,
            [this](bool checked) { onVersionToggled(checked); });
    connect(rawButton_, &QPushButton::toggled, this,
            [this](bool checked) { onVersionToggled(checked); });
    versionToggle_->hide();
    bar->addWidget(versionToggle_);

    zoomOutButton_ = makeZoomButton(QStringLiteral(u"−"), QStringLiteral("Smaller text (Ctrl+-)"));
    connect(zoomOutButton_, &QPushButton::clicked, this, [this] { zoom(-1); });
    zoomInButton_ = makeZoomButton(QStringLiteral("+"), QStringLiteral("Larger text (Ctrl++)"));
    connect(zoomInButton_, &QPushButton::clicked, this, [this] { zoom(1); });
    bar->addWidget(zoomOutButton_);
    bar->addWidget(zoomInButton_);

    copyButton_ = new QPushButton(QStringLiteral("Copy"));
    copyButton_->setObjectName(QStringLiteral("transcriptViewerCopyButton"));
    copyButton_->setCursor(Qt::PointingHandCursor);
    copyButton_->setToolTip(QStringLiteral("Copy the transcript as text (Ctrl+C)"));
    connect(copyButton_, &QPushButton::clicked, this, [this] { copyShownText(); });
    bar->addWidget(copyButton_);

    outer->addWidget(toolbar);

    view_ = new ReaderView;
    view_->setObjectName(QStringLiteral("transcriptViewerText"));
    connect(view_, &ReaderView::zoomRequested, this, [this](int direction) { zoom(direction); });
    outer->addWidget(view_, 1);

    refreshZoomButtons();

    struct Binding {
        QKeySequence keys;
        std::function<void()> handler;
    };
    const Binding bindings[] = {
        {QKeySequence(QKeySequence::Copy),
         [this] {
             copyShownText();
         }},
        {QKeySequence(QKeySequence::ZoomIn),
         [this] {
             zoom(1);
         }},
        {QKeySequence(QStringLiteral("Ctrl+=")),
         [this] {
             zoom(1);
         }},
        {QKeySequence(QKeySequence::ZoomOut),
         [this] {
             zoom(-1);
         }},
        {QKeySequence(QStringLiteral("Ctrl+0")),
         [this] {
             resetZoom();
         }},
    };
    for (const Binding& binding : bindings) {
        auto* shortcut = new QShortcut(binding.keys, this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, binding.handler);
    }
}

void TranscriptViewerDialog::setTranscript(const QString& text, const QString& raw,
                                           const QString& title) {
    setPages({Page{QStringLiteral("Transcript"), text, raw, QString()}}, title);
}

void TranscriptViewerDialog::setBatchResult(const dictation::BatchResult& result,
                                            const QString& overview, const QString& overviewRaw,
                                            const QString& title) {
    std::vector<const dictation::BatchItemResult*> completed;
    for (const dictation::BatchItemResult& item : result.items) {
        if (item.succeeded()) {
            completed.push_back(&item);
        }
    }
    if (completed.size() < 2) {
        setTranscript(overview, overviewRaw, title);
        return;
    }

    QList<Page> pages;
    pages.append(Page{QStringLiteral("Overview"), overview, overviewRaw,
                      QStringLiteral("All completed transcriptions")});
    int position = 1;
    for (const dictation::BatchItemResult* item : completed) {
        const QString text = formatSection(item->item, QString::fromStdString(item->text));
        QString raw;
        if (item->raw_text && !item->raw_text->empty()) {
            raw = formatSection(item->item, QString::fromStdString(*item->raw_text));
        }
        pages.append(Page{QStringLiteral("Trans. %1").arg(position), text, raw,
                          QString::fromStdString(item->item.source_name())});
        ++position;
    }

    bool cleanupRan =
        (result.combined_cleanup_provider && !result.combined_cleanup_provider->empty()) ||
        (result.combined_cleanup_model && !result.combined_cleanup_model->empty());
    for (const dictation::BatchItemResult* item : completed) {
        cleanupRan = cleanupRan || (item->cleanup_provider && !item->cleanup_provider->empty()) ||
                     (item->cleanup_model && !item->cleanup_model->empty());
    }
    if (cleanupRan) {
        pages.append(Page{QStringLiteral("AI Output"), overview, QString(),
                          QStringLiteral("Output after AI cleanup")});
    }
    setPages(pages, title);
}

void TranscriptViewerDialog::setPages(const QList<Page>& pages, const QString& title) {
    pages_ = pages;
    titleLabel_->setText(title.isEmpty() ? QStringLiteral("Transcript") : title);
    setWindowTitle(title.isEmpty() ? QStringLiteral("Transcript")
                                   : QStringLiteral(u"Transcript — %1").arg(title));

    pageTabs_->blockSignals(true);
    while (pageTabs_->count() > 0) {
        pageTabs_->removeTab(0);
    }
    for (const Page& page : pages) {
        const int index = pageTabs_->addTab(page.label);
        if (!page.tooltip.isEmpty()) {
            pageTabs_->setTabToolTip(index, page.tooltip);
        }
    }
    pageTabs_->setCurrentIndex(0);
    pageTabs_->blockSignals(false);
    const bool showTabs = pages.size() > 1;
    pageTabs_->setVisible(showTabs);
    toolbarSpacer_->setVisible(!showTabs);
    showPage(0);
}

void TranscriptViewerDialog::showPage(int index) {
    if (index < 0 || index >= pages_.size()) {
        return;
    }
    const Page& page = pages_[index];
    fixedText_ = page.text;
    rawText_ = (!page.raw.isEmpty() && page.raw != page.text) ? page.raw : QString();
    versionToggle_->setVisible(!rawText_.isEmpty());
    fixedButton_->blockSignals(true);
    rawButton_->blockSignals(true);
    fixedButton_->setChecked(true);
    rawButton_->setChecked(false);
    fixedButton_->blockSignals(false);
    rawButton_->blockSignals(false);
    showingRaw_ = false;
    render();
}

void TranscriptViewerDialog::clear() {
    setTranscript(QString());
}

QString TranscriptViewerDialog::shownText() const {
    if (showingRaw_ && !rawText_.isEmpty()) {
        return rawText_;
    }
    return fixedText_;
}

void TranscriptViewerDialog::zoom(int direction) {
    const int index = std::clamp(zoomIndex_ + direction, 0, kZoomStepCount - 1);
    if (index == zoomIndex_) {
        return;
    }
    zoomIndex_ = index;
    refreshZoomButtons();
    render(true);
}

void TranscriptViewerDialog::refreshTypography() {
    render(true);
}

void TranscriptViewerDialog::resetZoom() {
    if (zoomIndex_ == kDefaultZoomIndex) {
        return;
    }
    zoomIndex_ = kDefaultZoomIndex;
    refreshZoomButtons();
    render(true);
}

void TranscriptViewerDialog::refreshZoomButtons() {
    zoomOutButton_->setEnabled(zoomIndex_ > 0);
    zoomInButton_->setEnabled(zoomIndex_ < kZoomStepCount - 1);
}

void TranscriptViewerDialog::onVersionToggled(bool checked) {
    if (!checked) {
        return;
    }
    showingRaw_ = rawButton_->isChecked();
    render();
}

void TranscriptViewerDialog::render(bool keepPosition) {
    QScrollBar* scrollbar = view_->verticalScrollBar();
    double fraction = 0.0;
    if (keepPosition && scrollbar->maximum() > 0) {
        fraction = static_cast<double>(scrollbar->value()) / scrollbar->maximum();
    }
    ui::renderMarkdown(*view_->document(), shownText(),
                       ui::MarkdownStyle::READER.scaled(ui::currentUiFontScale() * zoomFactor()));
    copyButton_->setEnabled(!shownText().trimmed().isEmpty());
    if (keepPosition) {
        // Python's round(): half to even.
        scrollbar->setValue(static_cast<int>(std::nearbyint(fraction * scrollbar->maximum())));
    }
}

void TranscriptViewerDialog::copyShownText() {
    const QString text = shownText().trimmed();
    if (!text.isEmpty()) {
        Q_EMIT copyRequested(text);
    }
}

} // namespace fv
