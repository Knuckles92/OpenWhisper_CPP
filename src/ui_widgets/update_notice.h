#pragma once

#include <QPushButton>
#include <QString>

namespace fv {

// Title-bar button that stays up while a newer release is waiting (object name
// "updateNoticeButton"). Hidden until a check finds a newer release; clicking it opens the
// update dialog (the clicked signal). Affinity: GUI thread.
class UpdateNoticeButton : public QPushButton {
    Q_OBJECT
public:
    explicit UpdateNoticeButton(QWidget* parent = nullptr);

    // Offers version; an empty one hides the button.
    void showVersion(const QString& version);
    // Drops the version from the label; the compact window has no room. The tooltip keeps it.
    void setCompact(bool compact);

private:
    void syncText();

    QString version_;
    bool compact_ = false;
};

} // namespace fv
