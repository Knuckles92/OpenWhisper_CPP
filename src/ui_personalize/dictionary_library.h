#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "text/dictionary.h"

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace fv {

class Button;
class ElidingLabel;
class EmptyState;
class Reflow;
class WrappedLabel;

// Buttons side by side in a transparent holder (object name "dictionaryActions"), for Reflow.
QWidget* actionGroup(const std::vector<QWidget*>& buttons);

// One dictionary word (ui_qt/widgets/dictionary_library.py DictionaryRow, object name
// "dictionaryRow"): its star, the word on a line of its own, its badges with the "Sounds like"
// detail under it, and Edit and Remove. A learned word that is still new offers Undo instead of
// Remove. Terms are user content: never log them.
// Affinity: GUI thread.
class DictionaryRow : public QFrame {
    Q_OBJECT
public:
    explicit DictionaryRow(const text::DictionaryTerm& term, QWidget* parent = nullptr);

    const text::DictionaryTerm& term() const { return term_; }
    void setTerm(const text::DictionaryTerm& term);

    QPushButton* star() const { return star_; }
    ElidingLabel* name() const { return name_; }
    QLabel* learnedBadge() const { return learnedBadge_; }
    QLabel* newBadge() const { return newBadge_; }
    QLabel* keptBadge() const { return keptBadge_; }
    ElidingLabel* detail() const { return detail_; }
    Button* edit() const { return edit_; }
    Button* remove() const { return remove_; }
    Reflow* layoutRow() const { return layoutRow_; }

Q_SIGNALS:
    void starToggled(const QString& termId, bool starred);
    void editRequested(const QString& termId);
    void removeRequested(const QString& termId);

private:
    text::DictionaryTerm term_;
    QPushButton* star_ = nullptr;
    ElidingLabel* name_ = nullptr;
    QLabel* learnedBadge_ = nullptr;
    QLabel* newBadge_ = nullptr;
    QLabel* keptBadge_ = nullptr;
    ElidingLabel* detail_ = nullptr;
    Button* edit_ = nullptr;
    Button* remove_ = nullptr;
    Reflow* layoutRow_ = nullptr;
};

// Every saved word, newest first, with a search box once the list grows
// (ui_qt/widgets/dictionary_library.py DictionaryLibrary, object name "dictionaryLibrary").
//
// Builds at most kMaxRows rows, reusing them between updates, so a full dictionary stays quick to
// show; the search reaches the rest. Tab goes from the search box through each row's star, Edit
// and Remove in the order drawn. Removing a word by keyboard leaves focus on the word that takes
// its place, or, once no word is left to show, on the search box when it shows, else on
// `focusWhenEmpty`.
// Affinity: GUI thread.
class DictionaryLibrary : public QWidget {
    Q_OBJECT
public:
    static constexpr std::size_t kMaxRows = 100;
    // Words before the search box appears.
    static constexpr std::size_t kSearchFrom = 9;

    explicit DictionaryLibrary(QWidget* focusWhenEmpty = nullptr, QWidget* parent = nullptr);

    // The rows on show, top to bottom.
    std::vector<QString> visibleIds() const;
    DictionaryRow* row(const QString& termId) const;
    void setTerms(std::vector<text::DictionaryTerm> terms);

    QLineEdit* search() const { return search_; }
    EmptyState* empty() const { return empty_; }
    WrappedLabel* more() const { return more_; }

Q_SIGNALS:
    void starToggled(const QString& termId, bool starred);
    void editRequested(const QString& termId);
    void removeRequested(const QString& termId);

private:
    std::vector<text::DictionaryTerm> matching() const;
    std::optional<std::size_t> focusedRowIndex(const std::vector<QString>& keep) const;
    void placeRows(const std::vector<text::DictionaryTerm>& shown);
    void chainTabOrder(const std::vector<text::DictionaryTerm>& shown);
    void refocusAfterRemoval(std::size_t held, const std::vector<text::DictionaryTerm>& shown);
    void render();

    QPointer<QWidget> focusWhenEmpty_;
    std::vector<text::DictionaryTerm> terms_;
    std::map<QString, DictionaryRow*> rows_;
    QLineEdit* search_ = nullptr;
    QWidget* list_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    EmptyState* empty_ = nullptr;
    WrappedLabel* more_ = nullptr;
};

} // namespace fv
