#include "ui_widgets/list_reconcile.h"

#include <optional>

#include <QBoxLayout>
#include <QLayoutItem>
#include <QWidget>

namespace fv {

CardTable reconcileCards(QBoxLayout* layout, const std::vector<ReconcileItem>& items,
                         const CardTable& previous,
                         const std::function<QWidget*(std::size_t index)>& create,
                         const std::function<void(QWidget* card, std::size_t index)>& refresh) {
    std::map<std::string, Json> fingerprints;
    std::map<QWidget*, std::string> idOfCard;
    for (const auto& [id, entry] : previous) {
        fingerprints[id] = entry.fingerprint;
        idOfCard[entry.card] = id;
    }

    // The id of each layout item's card; an item that is not a known card (a stretch, a widget
    // this list never made) gets an id no record can have, so the plan removes it.
    std::vector<std::optional<std::string>> oldLayout;
    for (int index = 0; index < layout->count(); ++index) {
        QWidget* widget = layout->itemAt(index)->widget();
        const auto known = widget != nullptr ? idOfCard.find(widget) : idOfCard.end();
        if (widget == nullptr) {
            oldLayout.push_back(std::nullopt);
        } else if (known != idOfCard.end()) {
            oldLayout.push_back(known->second);
        } else {
            oldLayout.push_back(std::string("\0unknown-", 9) + std::to_string(index));
        }
    }

    const ReconcilePlan plan = reconcile_plan(oldLayout, fingerprints, items);

    std::vector<QWidget*> ordered;
    for (std::size_t index = 0; index < plan.entries.size(); ++index) {
        const ReconcilePlan::Entry& entry = plan.entries[index];
        if (entry.reused) {
            QWidget* card = previous.at(entry.id).card;
            if (refresh) {
                refresh(card, index);
            }
            ordered.push_back(card);
        } else {
            ordered.push_back(create(index));
        }
    }

    for (const std::size_t index : plan.removed) {
        QLayoutItem* item = layout->takeAt(static_cast<int>(index));
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    for (const std::size_t index : plan.inserted) {
        layout->insertWidget(static_cast<int>(index), ordered[index]);
    }

    CardTable current;
    for (std::size_t index = 0; index < plan.entries.size(); ++index) {
        // A repeated id keeps the last record's card, as Python's dict assignment does.
        const std::string& id = plan.entries[index].id;
        current[id] = {plan.fingerprints.at(id), ordered[index]};
    }
    return current;
}

} // namespace fv
