#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "base/json.h"
#include "uimath/list_reconcile.h"

class QBoxLayout;
class QWidget;

namespace fv {

// What a bounded list remembers about each card it shows: the fingerprint the card was drawn
// from, and the card.
struct CardEntry {
    Json fingerprint;
    QWidget* card = nullptr;
};
using CardTable = std::map<std::string, CardEntry>;

// Reuses the unchanged cards of a bounded list (history entries, past meetings) while keeping
// display order: a card whose id is in previous with an equal fingerprint stays (refresh is then
// called with the new record's index), any other record gets a card from create(index), and
// every card no longer wanted, and every non-card item such as a stretch, is hidden, unparented
// and deleted. Returns the table to pass as previous next time. Decisions come from
// reconcile_plan; this applies them to layout. Affinity: GUI thread.
CardTable reconcileCards(QBoxLayout* layout, const std::vector<ReconcileItem>& items,
                         const CardTable& previous,
                         const std::function<QWidget*(std::size_t index)>& create,
                         const std::function<void(QWidget* card, std::size_t index)>& refresh = {});

} // namespace fv
