#pragma once

#include <QPlainTextEdit>

namespace fv {

// The meeting brief field (meeting_mode_tab.py _BriefEdit), whose height is a count of text
// lines at the live font. The stylesheet supplies the font size, border and padding only once
// the field is polished, and again whenever the UI type size changes, so a pixel height fixed
// in the constructor would clip at a larger scale; Qt re-reads the size hint after each font or
// style change. Affinity: GUI thread.
class BriefEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    static constexpr int kVisibleLines = 3;

    explicit BriefEdit(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

} // namespace fv
