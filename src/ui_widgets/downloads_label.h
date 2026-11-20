#pragma once

#include "ui_widgets/eliding_label.h"

namespace fv {

// Engine messages with an in-app Downloads link that survives elision: a message ending
// " in Downloads." keeps "Downloads" as a link and shortens the explanation before it.
// Affinity: GUI thread.
class DownloadsLabel : public ElidingLabel {
    Q_OBJECT
public:
    explicit DownloadsLabel(const QString& text = {}, QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void downloadsRequested();

protected:
    void applyElide() override;

private:
    void onLinkActivated(const QString& link);
};

} // namespace fv
