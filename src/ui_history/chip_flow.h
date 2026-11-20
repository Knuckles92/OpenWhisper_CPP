#pragma once

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <QSize>
#include <QWidget>

namespace fv {

// _ChipFlow: chips in a line that wraps to another when they do not all fit. Eliding them all
// to share one line cut short chips down to "…" at large fonts; wrapping keeps each readable,
// and only a chip wider than a whole line is elided.
//
// Qt asks a History list's every card for its height many times per relayout, so the chips are
// measured once and measured again only after an event that can change their size (a layout
// request, show, a font or style change).
// Affinity: GUI thread.
class ChipFlow : public QWidget {
    Q_OBJECT
public:
    static constexpr int kSpacing = 6;
    // Heights remembered per width before the cache starts over.
    static constexpr std::size_t kHeightCacheSize = 8;

    // Takes the chips as children.
    explicit ChipFlow(std::vector<QWidget*> chips, QWidget* parent = nullptr);

    const std::vector<QWidget*>& chips() const { return chips_; }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Measure {
        std::vector<int> widths;
        int line = 0;
        friend bool operator==(const Measure&, const Measure&) = default;
    };

    const Measure& measure() const;
    // Lays the chips out in width (when apply) and returns the height they take.
    int arrange(int width, bool apply) const;

    std::vector<QWidget*> chips_;
    mutable std::optional<Measure> measured_;
    mutable std::map<int, int> heights_;
};

} // namespace fv
