#pragma once

#include <QPainterPath>
#include <QPushButton>
#include <QStringList>

namespace fv {

// "Manage host models", as a link on the engine card's footer line
// (ui_qt/widgets/remote_model_notice.py RemoteModelNotice). It rides that line so the Remote card
// stays as tall as every other backend's. When the host reports something it could install, a
// dot on the glyph says so and the tooltip names it. Affinity: GUI thread.
class RemoteModelNotice : public QPushButton {
    Q_OBJECT
public:
    // The glyph's box; the theme's left padding leaves room for it.
    static constexpr int kGlyphSize = 14;

    explicit RemoteModelNotice(QWidget* parent = nullptr);

    // What the host could install, by label; empty when nothing.
    void setAvailable(const QStringList& names);
    const QStringList& available() const { return available_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPainterPath glyph_;
    QStringList available_;
};

} // namespace fv
