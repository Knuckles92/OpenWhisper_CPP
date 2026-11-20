#pragma once

#include <QHash>
#include <QIcon>
#include <QListWidget>
#include <QSet>
#include <QString>
#include <QStringList>

class QListWidgetItem;

namespace fv {

// Left navigation rail (object name "modelManagerRail"): Home, then chapters whose pages report
// their value. Settings uses it instead of a tab bar.
//
// A chapter is a heading row that opens to show its pages. Where you are reads at two levels:
// the top-level row you are in (Home, or the current page's chapter) carries the selection pill
// (the "active" property on the chapter row, "selected" on a page), and the current page is
// marked on its chapter's guide line. Chapters open and close on their own, never because
// another one opened, so the row you click stays under the pointer; clicking the chapter you
// are in folds it. Each page shows its current value, elided, under its name. Keyboard steps
// (arrows, Page Up and Down by 5, Home, End) walk every page in order and open chapters as they
// go, so a heading is never the current item. Affinity: GUI thread.
class NavRail : public QListWidget {
    Q_OBJECT
public:
    static constexpr int kRailWidth = 258;
    static constexpr int kRailMinWidth = 220;
    // Keyboard steps a Page Up or Page Down moves.
    static constexpr int kPageStep = 5;

    explicit NavRail(QWidget* parent = nullptr);

    // Starts a chapter; the destinations added next are its pages.
    void addChapter(const QString& title, const QIcon& icon = {});
    // A page of the chapter added last, or a top-level page before any chapter.
    void addDestination(const QString& key, const QString& name, const QIcon& icon = {});

    // Shows text, elided, under the destination's name; an empty text leaves one line.
    void setValue(const QString& key, const QString& text);
    // The full value, before eliding; "" for an unknown key.
    QString value(const QString& key) const;
    // The destination's display name; "" for an unknown key.
    QString name(const QString& key) const;
    // The chapter a destination belongs to, or "" for a top-level page.
    QString chapter(const QString& key) const;
    QStringList chapters() const { return chapterOrder_; }
    QStringList chapterKeys(const QString& title) const;
    // The current page's chapter, or "" on a top-level page.
    QString activeChapter() const;
    bool isExpanded(const QString& title) const { return expanded_.contains(title); }
    // Opens or closes one chapter, leaving every other one as it is.
    void setExpanded(const QString& title, bool expanded);
    // Makes key current, opening its chapter.
    void select(const QString& key);
    // Opens a chapter on the page it was last showing, else its first.
    void selectChapter(const QString& title);
    // "" when no page is current.
    QString currentKey() const;
    // Every destination, in rail order.
    QStringList keys() const { return keyOrder_; }

    // The list item and row widget of a destination or a chapter heading; nullptr when unknown.
    QListWidgetItem* destinationItem(const QString& key) const { return items_.value(key); }
    QWidget* destinationRow(const QString& key) const;
    QListWidgetItem* chapterItem(const QString& title) const;
    QWidget* chapterRow(const QString& title) const;

Q_SIGNALS:
    void destinationChanged(const QString& key);

protected:
    void updateGeometries() override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;

private:
    struct Chapter {
        QListWidgetItem* item = nullptr;
        QWidget* row = nullptr;
        QStringList keys;
        // The page the chapter reopens on.
        QString last;
    };

    // Goes to a chapter, or folds it away when it is already where you are. A closed chapter
    // opens on its page; an open one you are elsewhere than takes you there, so a click on a
    // heading never hides the pages you meant to reach.
    void clickChapter(const QString& title);
    void onCurrentChanged(QListWidgetItem* current);

    QHash<QString, QWidget*> rows_;
    QHash<QString, QListWidgetItem*> items_;
    QStringList keyOrder_;
    QHash<QString, Chapter> chapters_;
    QStringList chapterOrder_;
    QHash<QString, QString> chapterOf_;
    // The chapter new destinations join; "" adds top-level pages.
    QString adding_;
    QSet<QString> expanded_;
};

} // namespace fv
