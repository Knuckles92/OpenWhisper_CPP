#pragma once

#include <optional>
#include <vector>

#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

class QBoxLayout;

// A control column beside a stage: the layout the Walkthrough and Meeting tabs share. The column
// holds what you set; the stage shows what is happening. Neither moves the other, and the stage
// changing what it shows never resizes the window.
namespace fv {

namespace studio {
// Sizes at 100% type; the stage's grow with the font scale (ui::scaled).
// The control column.
inline constexpr int kColumnWidth = 320;
// Between the column and the stage.
inline constexpr int kGap = 16;
// The stage at its smallest, and the width it asks for beside the column.
inline constexpr QSize kStageMin{400, 520};
inline constexpr int kStageWidth = 704;
} // namespace studio

// The right side's one slot: whichever pane shows fills it.
//
// It has no layout on purpose. A layout would pass each pane's size hint up to the window,
// which would then resize as the stage went from one pane to the next. Affinity: GUI thread.
class Stage : public QWidget {
    Q_OBJECT
public:
    // height is at 100% type and grows with it; without it the stage keeps the fixed minimum
    // height.
    explicit Stage(const QString& objectName = QStringLiteral("walkthroughStage"),
                   int width = studio::kStageWidth, std::optional<int> height = std::nullopt);

    // Takes pane into the stage, hidden.
    QWidget* addPane(QWidget* pane);
    // Shows pane alone, filling the stage.
    void showPane(QWidget* pane);
    QWidget* current() const { return current_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    int stageHeight() const;

    int width_;
    std::optional<int> height_;
    // A pane its page deleted drops out instead of dangling.
    std::vector<QPointer<QWidget>> panes_;
    QPointer<QWidget> current_;
};

// The control column beside the stage, or above it once both do not fit: below column width +
// 16 + the stage's minimum width.
//
// Its own minimum width is ignored, as Reflow's is: the width comes from the page, so a narrow
// tiled window stacks the two instead of clipping the stage at the right edge. Affinity: GUI
// thread.
class StudioSplit : public QWidget {
    Q_OBJECT
public:
    StudioSplit(QWidget* column, Stage* stage, QWidget* parent = nullptr,
                const QString& objectName = QStringLiteral("walkthroughStudio"));

    QWidget* column() const { return column_; }
    Stage* stage() const { return stage_; }
    int columnWidth() const { return columnWidth_; }
    bool isStacked() const { return stacked_.value_or(false); }

    void setColumnWidth(int width);
    // placeForWidth at the current width.
    void refit();
    // Lays the two out as they sit once this is width wide. The window calls it before
    // measuring a page it is about to widen, so it measures the layout it will show and goes
    // there in one move.
    void placeForWidth(int width);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void place(bool stacked);

    QWidget* column_;
    Stage* stage_;
    int columnWidth_ = studio::kColumnWidth;
    QBoxLayout* box_;
    std::optional<bool> stacked_;
};

} // namespace fv
