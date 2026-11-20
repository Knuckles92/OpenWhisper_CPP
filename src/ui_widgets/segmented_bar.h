#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <QPushButton>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QTimer;
class QVBoxLayout;

namespace fv {

// One choice of a SegmentedBar (object name "mcpChoice"): a native, keyboard-accessible
// checkable button whose title and optional detail are child labels ("mcpChoiceTitle",
// "mcpChoiceDetail"), each sized on its own, so the detail can change while shown. The first
// label carries the "selected" property while the button is checked. Affinity: GUI thread.
class SegmentButton : public QPushButton {
    Q_OBJECT
public:
    SegmentButton(const QString& title, const QString& detail, bool compact = false,
                  QWidget* parent = nullptr);

    // The main line, which may change while shown (a count, for one-line tabs).
    void setTitle(const QString& text);
    // The supporting line under the title, which may change while shown.
    void setDetail(const QString& text);
    QLabel* titleLabel() const { return title_; }
    QLabel* detailLabel() const { return detail_; }

    // The width with every line on one line; measured again after a change.
    int naturalWidth();
    // Lets the lines wrap, for a tray too narrow for even one choice per row. Wrapping, the
    // button takes the tray's width and is as tall as its text needs at that width; otherwise it
    // keeps one line per text and is as wide as its longest.
    void setWrapping(bool wrap);
    bool isWrapping() const { return wrapping_; }

    // Like WrappedLabel: the size hints carry the height for the current width, so the page's
    // minimum height stays honest.
    bool hasHeightForWidth() const override { return false; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* line(const QString& text, const char* objectName);

    bool compact_;
    QVBoxLayout* layout_;
    QLabel* title_ = nullptr;
    QLabel* detail_ = nullptr;
    bool wrapping_ = false;
    std::optional<int> natural_;
};

// A segmented tray (object name "mcpChoiceBar"): equal-width choices that wrap into balanced
// rows (segmented_columns) instead of one tall stack, and into one column with wrapped text when
// not even one fits per row. Used for the MCP page's computer and assistant choosers, the
// Remote engine page's tabs and the overlay looks. Affinity: GUI thread.
class SegmentedBar : public QWidget {
    Q_OBJECT
public:
    // items are (title, detail); an empty detail leaves the second line out.
    explicit SegmentedBar(const std::vector<std::pair<QString, QString>>& items,
                          bool compact = false, QWidget* parent = nullptr);

    const std::vector<SegmentButton*>& buttons() const { return buttons_; }
    int currentIndex() const { return index_; }
    // Checks the choice at index and emits currentIndexChanged, never activated. An index out
    // of range or already current does nothing.
    void setCurrentIndex(int index);
    void setDetail(int index, const QString& text);
    void setTitle(int index, const QString& text);
    // Choices per row as now laid out.
    int columns() const { return columns_; }

Q_SIGNALS:
    void currentIndexChanged(int index);
    // Only a person's click, not setCurrentIndex called by the page, so loading a page never
    // saves.
    void activated(int index);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void place(int columns);
    void reflow();

    QButtonGroup* group_;
    int index_ = -1;
    int columns_ = 0;
    std::vector<SegmentButton*> buttons_;
    QTimer* reflowTimer_ = nullptr;
};

} // namespace fv
