#pragma once

#include <QScrollArea>

class QDialog;
class QVBoxLayout;

namespace fv {

// A frameless, vertical-only scroll area for a dialog's explanation (object name
// "dialogScrollingBody", its content "dialogScrollingBodyContent").
//
// A wrapped QLabel placed straight in a dialog layout reports a height for a guessed width, so
// long copy got less room than it needed and was clipped at both ends. Here the dialog grows to
// the copy's real height at its real width (fitDialog), up to maxHeight, and a scroll bar takes
// over past that, so every line stays readable. Affinity: GUI thread.
class ScrollingBody : public QScrollArea {
    Q_OBJECT
public:
    // The tallest the copy grows before it scrolls, at 100% font.
    static constexpr int kDefaultMaxHeight = 420;
    // Room kept for the window frame and a desktop panel.
    static constexpr int kScreenMargin = 160;
    static constexpr int kMinimumHeight = 96;

    explicit ScrollingBody(int maxHeight = kDefaultMaxHeight, QWidget* parent = nullptr);

    QWidget* content() const { return content_; }
    QVBoxLayout* contentLayout() const { return contentLayout_; }
    void addWidget(QWidget* widget);

    // Resizes dialog so the copy shows in full, up to maxHeight and the screen's available
    // height less kScreenMargin, never below kMinimumHeight. Call before the dialog is first
    // shown, after its width is final.
    void fitDialog(QDialog* dialog);

private:
    // The copy's height at width, measured line by line. Wrapped labels are asked for their
    // height at that width directly: their size hints follow the width they last had, which a
    // dialog that has not been shown yet has not given them.
    int contentHeight(int width) const;

    int maxHeight_;
    QWidget* content_;
    QVBoxLayout* contentLayout_;
};

// body->fitDialog(dialog).
void fitDialog(QDialog* dialog, ScrollingBody* body);

} // namespace fv
