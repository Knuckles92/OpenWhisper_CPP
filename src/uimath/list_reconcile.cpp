#include "uimath/list_reconcile.h"

#include <algorithm>
#include <cstddef>
#include <variant>

#include "pyport/json.h"

namespace fv {
namespace {

// A card: the one already in the layout for an id, or the n-th card created by this plan.
struct OldCard {
    std::string id;
    friend bool operator==(const OldCard&, const OldCard&) = default;
};
struct NewCard {
    std::size_t serial;
    friend bool operator==(const NewCard&, const NewCard&) = default;
};
using Card = std::variant<OldCard, NewCard>;

} // namespace

ReconcilePlan reconcile_plan(const std::vector<std::optional<std::string>>& old_layout,
                             const std::map<std::string, Json>& previous,
                             const std::vector<ReconcileItem>& new_items) {
    ReconcilePlan plan;
    std::vector<Card> ordered;
    std::size_t created = 0;
    for (const ReconcileItem& item : new_items) {
        const auto old = previous.find(item.id);
        const bool reused = old != previous.end() && py::py_equal(old->second, item.fingerprint);
        if (reused) {
            ordered.emplace_back(OldCard{item.id});
        } else {
            ordered.emplace_back(NewCard{created++});
        }
        plan.entries.push_back({item.id, reused});
        plan.fingerprints[item.id] = item.fingerprint;
    }

    const auto wanted = [&](const Card& card) {
        return std::find(ordered.begin(), ordered.end(), card) != ordered.end();
    };
    std::vector<Card> layout;
    for (const std::optional<std::string>& id : old_layout) {
        // A layout item that is not a card can never be wanted; it is removed with the stale
        // cards. An old card is identified by its id, as previous maps an id to its card.
        layout.emplace_back(id ? Card{OldCard{*id}} : Card{NewCard{static_cast<std::size_t>(-1)}});
    }
    for (std::size_t index = layout.size(); index-- > 0;) {
        if (!wanted(layout[index])) {
            plan.removed.push_back(index);
            layout.erase(layout.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const Card& card = ordered[index];
        if (index < layout.size() && layout[index] == card) {
            continue;
        }
        plan.inserted.push_back(index);
        // insertWidget on a card the layout already holds takes it out of its old position
        // first (QLayout::addChildWidget).
        const auto existing = std::find(layout.begin(), layout.end(), card);
        if (existing != layout.end()) {
            layout.erase(existing);
        }
        const std::size_t at = std::min(index, layout.size());
        layout.insert(layout.begin() + static_cast<std::ptrdiff_t>(at), card);
    }
    return plan;
}

} // namespace fv
