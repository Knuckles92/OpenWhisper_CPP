#pragma once

#include <QFrame>
#include <QString>

class QLabel;

namespace fv {

class Button;

// The stylesheet source (with @tokens, for ui::setLocalStyleSheet) of one Downloads row: the
// card, its name, summary, size and badge labels, the compact buttons and the progress bar, for
// the object names the row uses. Labels are "<row>Name", "<row>Summary", "<row>Size" and
// "<row>Badge". `stateRules` are extra frame states placed before [selected] so a selected row
// wins over them; `extra` is appended after the shared rules.
QString rowStyle(const QString& row, const QString& primary, const QString& remove,
                 const QString& progress, const QString& secondary = {},
                 const QString& stateRules = {}, const QString& extra = {});

// A clickable Downloads row that opens its details popup. A click on the row body, or Enter,
// Return or Space while it has focus, emits detailsRequested with the row's id; a click on a
// button (the batch checkbox included) does not. Affinity: GUI thread.
class DownloadRow : public QFrame {
    Q_OBJECT
public:
    // The height every compact action button is fixed to.
    static constexpr int kButtonHeight = 28;

    const QString& rowId() const { return rowId_; }
    // The status chip; its tone property selects the QSS rule.
    QLabel* badge() const { return badge_; }

Q_SIGNALS:
    void detailsRequested(const QString& rowId);

protected:
    DownloadRow(const QString& rowId, const QString& objectName, const QString& style,
                const QString& tooltip, const QString& accessibleName,
                const QString& accessibleDescription, QWidget* parent = nullptr);

    // Fixes a shared application button to the row's 28 px action size and width.
    static void compactButton(Button* button, int width);
    // Badge text and the tone property the QSS selects on.
    void setBadge(const QString& text, const QString& tone);

    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    // Created by the subclass.
    QLabel* badge_ = nullptr;

private:
    // Whether widget, or an ancestor of it, is a button.
    static bool isActionChild(QWidget* widget);

    QString rowId_;
};

} // namespace fv
