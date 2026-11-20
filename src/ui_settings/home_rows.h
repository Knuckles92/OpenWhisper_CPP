#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <QString>

#include "base/json.h"

namespace fv {

class HomePage;

// The Home cards other pages add rows to. OpenWhisper builds the Dictation card's cleanup rows
// and the Meetings card inline and asks each Personalize page module for its home_rows; here the
// AI cleanup page (M4), the meeting pages (M7) and the Personalize modules contribute their rows
// through HomeRows, the same way.
enum class HomeCardId { Dictation, Personalize, Meetings };

// One entry of Home's where strip: `text` under "Stays on this computer", or under "Leaves this
// computer, only when used" when `leaves`. Entries are listed by `order` (then as contributed);
// HomeRows::kWhere* are the places OpenWhisper's items take (_refresh_home). Home composes the
// dictation voice, AI cleanup, text near the cursor, Learned rules and Recordings items itself;
// the meeting pages add the meeting voice, speaker labels and meeting intelligence ones.
struct HomeWhereItem {
    int order = 0;
    QString text;
    bool leaves = false;
};

// What other features add to Home, provided with ctx.provide<HomeRows>() by the settings
// feature (registerHomePage). A contributor runs each time a Settings window builds its Home:
// Dictation contributors after the Voice model row (where OpenWhisper's cleanup rows sit),
// Personalize and Meetings contributors before their card's closing link, each card's in the
// order they were added. A contributor adds rows with HomePage::addRow, toggle or bind (the
// projection of a page's own control) and can ask for HomePage::addRefreshHook. A card with no
// row whose link leads to no registered page is hidden.
// Affinity: GUI thread.
class HomeRows {
public:
    using Contributor = std::function<void(HomePage& home)>;
    // The where-strip entries of a settings snapshot.
    using WhereSource = std::function<std::vector<HomeWhereItem>(const Json& settings)>;

    // Where OpenWhisper lists its where-strip items, so a contribution lands in its place.
    static constexpr int kWhereDictationVoice = 10;
    static constexpr int kWhereMeetingVoice = 20;
    static constexpr int kWhereSpeakerLabels = 30;
    static constexpr int kWhereCleanup = 40;
    static constexpr int kWhereTextNearCursor = 50;
    static constexpr int kWhereMeetingIntelligence = 60;
    static constexpr int kWhereLearnedRules = 70;
    static constexpr int kWhereRecordings = 80;

    void addRows(HomeCardId card, Contributor contributor) {
        contributors_.emplace_back(card, std::move(contributor));
    }
    void addWhereItems(WhereSource source) { whereSources_.push_back(std::move(source)); }

    // The contributors of `card`, in the order they were added.
    std::vector<Contributor> contributors(HomeCardId card) const {
        std::vector<Contributor> found;
        for (const auto& [id, contributor] : contributors_) {
            if (id == card) {
                found.push_back(contributor);
            }
        }
        return found;
    }
    const std::vector<WhereSource>& whereSources() const { return whereSources_; }

private:
    std::vector<std::pair<HomeCardId, Contributor>> contributors_;
    std::vector<WhereSource> whereSources_;
};

} // namespace fv
