#pragma once

#include <QDialog>
#include <QList>
#include <QMargins>
#include <QString>
#include <QTextBrowser>

#include "dictation/batch.h"

class QButtonGroup;
class QPushButton;
class QTabBar;
class QWidget;

namespace fv {

class ElidingLabel;

// Text browser that keeps its column no wider than a comfortable measure. The document is laid
// out to the viewport, so a maximized window would run lines the full width of the screen. The
// view instead grows its viewport margins as it widens, centering a column capped at kMaxMeasure
// pixels. Ctrl+wheel asks for the dialog's zoom steps instead of Qt's own font zoom, which the
// Markdown renderer would override on the next render. Affinity: GUI thread.
class ReaderView : public QTextBrowser {
    Q_OBJECT
public:
    static constexpr int kMaxMeasure = 780;
    static constexpr int kMinSideMargin = 28;
    static constexpr int kTopMargin = 20;
    static constexpr int kBottomMargin = 32;

    explicit ReaderView(QWidget* parent = nullptr);

    // The margins the last resize gave the viewport.
    QMargins readerMargins() const { return viewportMargins(); }

Q_SIGNALS:
    // +1 for a wheel step up, -1 down.
    void zoomRequested(int direction);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
};

// Non-modal window that shows transcripts as formatted Markdown. The owning tab keeps a single
// instance and refreshes it through setTranscript or setBatchResult as results arrive, so the
// window follows the tab rather than freezing on the result it opened with. A batch with several
// completed files gets an Overview and one page per file, plus an AI Output page when cleanup
// ran. Text size and window geometry persist for the session because the dialog is hidden on
// close, not destroyed. An empty raw text means there is none. Affinity: GUI thread.
class TranscriptViewerDialog : public QDialog {
    Q_OBJECT
public:
    // ZOOM_STEPS and DEFAULT_ZOOM_INDEX.
    static constexpr double kZoomSteps[] = {0.85, 1.0, 1.15, 1.3, 1.5, 1.75};
    static constexpr int kZoomStepCount = 6;
    static constexpr int kDefaultZoomIndex = 1;

    explicit TranscriptViewerDialog(QWidget* parent = nullptr);

    // Shows text, with raw behind the Raw switch when it differs. The window title is
    // "Transcript — {title}", or "Transcript" without one.
    void setTranscript(const QString& text, const QString& raw = {}, const QString& title = {});
    // Navigation for every completed transcript in result. The Overview page is the exact
    // document the tab shows; each completed item gets a "Trans. N" page named after its file;
    // AI Output (the overview, no raw) is added only when a cleanup provider or model was
    // recorded. Fewer than two completed items show the overview alone.
    void setBatchResult(const dictation::BatchResult& result, const QString& overview,
                        const QString& overviewRaw = {}, const QString& title = {});
    // setTranscript("").
    void clear();
    // The Raw text only while Raw is selected and exists; else the Fixed text.
    QString shownText() const;

    double zoomFactor() const { return kZoomSteps[zoomIndex_]; }
    // Moves one step (+1 larger, -1 smaller) within the steps and re-renders keeping the scroll
    // position.
    void zoom(int direction);
    void resetZoom();
    // Re-renders keeping the scroll position, for a change of the UI font scale.
    void refreshTypography();

    ElidingLabel* titleLabel() const { return titleLabel_; }
    QTabBar* pageTabs() const { return pageTabs_; }
    QWidget* versionToggle() const { return versionToggle_; }
    QPushButton* fixedButton() const { return fixedButton_; }
    QPushButton* rawButton() const { return rawButton_; }
    QPushButton* zoomOutButton() const { return zoomOutButton_; }
    QPushButton* zoomInButton() const { return zoomInButton_; }
    QPushButton* copyButton() const { return copyButton_; }
    ReaderView* view() const { return view_; }

Q_SIGNALS:
    // The shown Markdown source, stripped (not its rendering).
    void copyRequested(const QString& text);

private:
    struct Page {
        QString label;
        QString text;
        QString raw;
        QString tooltip;
    };

    void setPages(const QList<Page>& pages, const QString& title);
    void showPage(int index);
    void refreshZoomButtons();
    void onVersionToggled(bool checked);
    void render(bool keepPosition = false);
    void copyShownText();

    QString fixedText_;
    QString rawText_;
    bool showingRaw_ = false;
    int zoomIndex_ = kDefaultZoomIndex;
    QList<Page> pages_;

    ElidingLabel* titleLabel_;
    QTabBar* pageTabs_;
    QWidget* toolbarSpacer_;
    QWidget* versionToggle_;
    QButtonGroup* versionGroup_;
    QPushButton* fixedButton_;
    QPushButton* rawButton_;
    QPushButton* zoomOutButton_;
    QPushButton* zoomInButton_;
    QPushButton* copyButton_;
    ReaderView* view_;
};

} // namespace fv
