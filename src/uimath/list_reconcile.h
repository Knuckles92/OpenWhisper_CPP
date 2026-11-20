#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"

namespace fv {

// One record of a bounded list (history entries, past meetings) and the fingerprint its card
// was drawn from.
struct ReconcileItem {
    std::string id;
    Json fingerprint;
};

// What reconcile_cards does to a layout of cards, decided without widgets.
struct ReconcilePlan {
    struct Entry {
        std::string id;
        // true: the card already showing this id is kept (its fingerprint is unchanged);
        // false: a new card is created for it.
        bool reused = false;
    };
    // The cards in display order, one per new item.
    std::vector<Entry> entries;
    // Positions in the old layout whose item is taken out (hidden, unparented, deleted), last
    // position first, the order in which they are removed.
    std::vector<std::size_t> removed;
    // Display positions at which the applier calls insertWidget, in order, after the removals:
    // a position whose layout item is not already the right card.
    std::vector<std::size_t> inserted;
    // The new id -> fingerprint table to keep for the next reconcile. A repeated id keeps the
    // last item's fingerprint, as Python's dict assignment does.
    std::map<std::string, Json> fingerprints;
};

// The plan for showing new_items where the layout holds old_layout: the id of each layout
// item's card in layout order, nullopt for an item that is not a card (a stretch), and
// previous, the fingerprint each shown id was drawn from. A card is reused when its id is in
// previous with an equal fingerprint (Python's ==); a repeated id reuses or creates one card.
ReconcilePlan reconcile_plan(const std::vector<std::optional<std::string>>& old_layout,
                             const std::map<std::string, Json>& previous,
                             const std::vector<ReconcileItem>& new_items);

} // namespace fv
