#pragma once

#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include <QMetaType>
#include <QString>
#include <QWidget>

#include "appcore/settings_search.h"

class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace fv {

class SettingsDialog;

// One palette result (settings_search.py _ResultRow): an icon chip, the title and the detail with
// every query word tinted (a help text cut to its snippet), each elided to the row's width with
// the full text as its tooltip, and badge pills ("Downloaded" and "Installed" read as ok). The
// list item under it takes the clicks.
// Affinity: GUI thread.
class SearchResultRow : public QWidget {
    Q_OBJECT
public:
    SearchResultRow(const appcore::SearchEntry& entry, std::vector<std::string> tokens,
                    QWidget* parent = nullptr);

    QLabel* titleLabel() const { return title_; }
    QLabel* detailLabel() const { return detail_; }
    // The texts before eliding.
    const QString& titleText() const { return titleText_; }
    const QString& detailText() const { return detailText_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    std::vector<std::string> tokens_;
    std::string color_;
    QLabel* title_;
    QLabel* detail_;
    QString titleText_;
    QString detailText_;
};

// The command palette over the Settings window (settings_search.py SearchPalette, object name
// "settingsSearchOverlay"): a scrim across the window and a panel kPanelWidth wide (narrower in
// a narrow window) with the query field, the results by section (Settings, Models & downloads,
// Help text; the section headers cannot be selected), an empty line and a footer of key hints
// and the result count. The index is asked for anew on every open. Up and Down move through the
// results, wrapping and skipping headers; Enter or a click opens one; Esc or a click outside the
// panel closes the palette and leaves Settings open.
// Affinity: GUI thread.
class SearchPalette : public QWidget {
    Q_OBJECT
public:
    using IndexProvider = std::function<std::vector<appcore::SearchEntry>()>;

    static constexpr int kPanelWidth = 640;
    static constexpr int kResultsMaxHeight = 380;
    // Below this panel width or window height the footer is hidden.
    static constexpr int kFooterMinWidth = 480;
    static constexpr int kFooterMinHeight = 280;
    // The scrim's alpha over slate-rail, 0-255.
    static constexpr int kScrimAlpha = 170;

    SearchPalette(QWidget* host, IndexProvider index);

    void open(const QString& text = {});
    void closePalette();

    // The entries listed now, in order (headers excluded).
    const std::vector<appcore::SearchEntry>& currentEntries() const { return listed_; }
    // The entry an item of results() holds; null for a section header.
    const appcore::SearchEntry* entryOf(const QListWidgetItem* item) const;

    QFrame* panel() const { return panel_; }
    QLineEdit* input() const { return input_; }
    QListWidget* results() const { return results_; }
    QLabel* emptyLabel() const { return empty_; }
    QWidget* footer() const { return footer_; }
    QLabel* countLabel() const { return count_; }

Q_SIGNALS:
    void activated(const fv::appcore::SearchEntry& entry);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void place();
    void runQuery(const QString& text);
    void move(int step);
    void activateItem(QListWidgetItem* item);

    QWidget* host_;
    IndexProvider index_;
    std::vector<appcore::SearchEntry> entries_;
    std::vector<appcore::SearchEntry> listed_;
    int resultHeight_ = 0;
    QFrame* panel_;
    QLineEdit* input_;
    QListWidget* results_;
    QLabel* empty_;
    QWidget* footer_;
    QLabel* count_;
};

// The legacy-name results (settings_dialog.py _SEARCH_ALIASES): destination, title, detail and
// keywords. Only those whose destination is registered are listed.
struct SearchAlias {
    std::string_view destination;
    const char* title;
    const char* detail;
    const char* keywords;
};
const std::vector<SearchAlias>& searchAliases();

// keyword_entries: one Setting entry per alias.
std::vector<appcore::SearchEntry> keywordEntries(const std::vector<SearchAlias>& aliases);

// shortcut_text: the platform's spelling of a shortcut, for the search button's hint.
QString shortcutText(const QString& sequence = QStringLiteral("Ctrl+K"));

// _search_index: every registered page (its heading, its visible tiles outside a part marked
// settingsSearch=false, de-duplicated by title, its field groups and footnotes), the speech
// models and components when Downloads is registered, the aliases of registered pages, then the
// registered search fields and help notes of each page not built yet. A tile's entry targets the
// tile; an unbuilt page's field names its control by target_name.
std::vector<appcore::SearchEntry> buildSearchIndex(SettingsDialog& dialog);

// _on_search_activated: a model opens Downloads on it, a component opens Downloads focused on
// it, anything else selects its destination and reveals its target.
void activateSearchEntry(SettingsDialog& dialog, const appcore::SearchEntry& entry);

// Puts a palette over `dialog` and makes it the window's search: the opener (Ctrl+K, Find, the
// search button, openSearch), the closer (closing, a backup starting) and the activation.
SearchPalette* installSearchPalette(SettingsDialog& dialog);

} // namespace fv

Q_DECLARE_METATYPE(fv::appcore::SearchEntry)
