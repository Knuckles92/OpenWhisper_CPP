#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QDate>
#include <QFrame>
#include <QImage>
#include <QSize>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace fv {

class Executor;

namespace walkthrough {
struct Walkthrough;
}

// The side panel's Walkthroughs page (ui_qt/widgets/past_walkthroughs.py), shown while the
// Walkthrough tab is open. Each card is one capture folder: when it was made, its title (the
// write-up's heading, else its first words), its first screenshot, and what it holds. Clicking
// one opens it in Saved Walkthroughs. Folders are read and the screenshots decoded off the GUI
// thread.
namespace past_walkthroughs {

// The most cards the page builds; Saved Walkthroughs lists every one.
inline constexpr int MAX_SHOWN = 60;
// The screenshot strip on each card, in logical pixels.
inline constexpr QSize THUMB_SIZE{296, 100};
// Titles made from spoken words stop around here (code points), at a word break.
inline constexpr std::size_t TITLE_CHARS = 90;

// What a card shows about one walkthrough folder.
struct Summary {
    std::filesystem::path folder;
    std::optional<std::chrono::local_seconds> when;
    std::string title;
    std::string meta;
    std::string pill;
    std::string pill_tone;
    // Everything a search matches against, lower-cased.
    std::string words;
    // Null: no screenshot to show.
    QImage thumb;
};

// The write-up's first "# heading" in `folder` (write-up.md), else the sections' words with their
// whitespace collapsed, cut at TITLE_CHARS on a word boundary with "…"; "No words" when nothing
// was said (_title).
std::string title(const std::filesystem::path& folder, const walkthrough::Walkthrough& document);

// The screenshot at `path` filling `box` (logical pixels at `ratio`), cover-scaled while it is
// decoded (never at full size), cropped from its top and centred across, corners rounded by
// `radius`; null when it cannot be read (_thumb).
QImage thumb(const std::filesystem::path& path, double ratio, QSize box = THUMB_SIZE,
             double radius = 8);

// Reads one capture folder for its card (summarize). `thumbSize` and `thumbRadius` shape its
// first screenshot, for a list other than this panel's (the Walkthrough tab's recent
// walkthroughs). Any thread.
Summary summarize(const std::filesystem::path& folder, std::chrono::year_month_day today,
                  double ratio = 1.0, QSize thumbSize = THUMB_SIZE, double thumbRadius = 8);

// What the page shows for a search (_filter): which cards match (every whitespace-separated,
// lower-cased term of `search` is in the card's words), the count line and the message under the
// cards. Before the first read (`total` nullopt) only the cards' visibility is decided.
struct FilterView {
    std::vector<bool> visible;
    bool counted = false; // false before the first read: the lines keep what they said
    std::string count;
    bool count_shown = false;
    std::string message;
    bool message_shown = false;
};
FilterView filter(std::span<const std::string> words, std::string_view search,
                  std::optional<int> total);

// QDate as the C++ calendar date.
std::chrono::year_month_day toDate(const QDate& date);

} // namespace past_walkthroughs

// One walkthrough: click, Enter or Space opens it. Affinity: GUI thread.
class WalkthroughCard : public QFrame {
    Q_OBJECT
public:
    WalkthroughCard(past_walkthroughs::Summary summary, std::chrono::year_month_day today,
                    QWidget* parent = nullptr);

    const past_walkthroughs::Summary& summary() const { return summary_; }
    QLabel* whenLabel() const { return whenLabel_; }
    QLabel* titleLabel() const { return titleLabel_; }

Q_SIGNALS:
    void clicked(const QString& folder);

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    past_walkthroughs::Summary summary_;
    QLabel* whenLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
};

// Past walkthroughs, newest first, for the side panel. Affinity: GUI thread.
class PastWalkthroughsPanel : public QWidget {
    Q_OBJECT
public:
    // `root` is the captures folder; `today` the date cards are told against (empty:
    // QDate::currentDate).
    PastWalkthroughsPanel(std::filesystem::path root, std::function<QDate()> today = {},
                          QWidget* parent = nullptr);
    ~PastWalkthroughsPanel() override;

    // Reads run on `io` (null: a thread of their own, as Python's daemon thread). It must outlive
    // the reads it was given.
    void setExecutor(Executor* io);
    // At most this many cards are built (MAX_SHOWN unless set).
    void setMaxShown(int count);

    // Reads the capture folders again, off the GUI thread; the newest read wins.
    void refresh();

    const std::filesystem::path& root() const { return root_; }
    const std::vector<WalkthroughCard*>& cards() const { return cards_; }
    // How many folders the last read found; nullopt until one has finished.
    std::optional<int> total() const { return total_; }
    QPushButton* libraryButton() const { return libraryButton_; }
    QLineEdit* searchInput() const { return searchInput_; }
    QScrollArea* scrollArea() const { return scrollArea_; }
    QLabel* countHeader() const { return countHeader_; }
    QLabel* emptyLabel() const { return emptyLabel_; }

Q_SIGNALS:
    // Open Saved Walkthroughs: a capture folder to select, or "" for the newest.
    void walkthroughSelected(const QString& folder);

private:
    struct Inbox;

    void setupUi();
    void loaded(std::uint64_t token, std::vector<past_walkthroughs::Summary> summaries, int total);
    void applyFilter();
    std::chrono::year_month_day today() const;

    std::filesystem::path root_;
    std::function<QDate()> today_;
    Executor* io_ = nullptr;
    int maxShown_ = past_walkthroughs::MAX_SHOWN;
    std::uint64_t token_ = 0;
    std::uint64_t issued_ = 0;
    std::optional<int> total_;
    std::shared_ptr<Inbox> inbox_;
    std::vector<WalkthroughCard*> cards_;

    QPushButton* libraryButton_ = nullptr;
    QLineEdit* searchInput_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QLabel* countHeader_ = nullptr;
    QVBoxLayout* cardLayout_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
};

} // namespace fv
