#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include <QMenu>

#include "appkit/registries.h"

namespace fv {

namespace {

// The role is always set: under QAction's default TextHeuristicRole Qt renames an item called
// "Settings" (main_window.py, _setup_menu).
QAction* build_action(const MenuItem& item, QObject* parent) {
    auto* action = new QAction(item.text, parent);
    if (!item.shortcut.isEmpty()) {
        action->setShortcut(item.shortcut);
    }
    action->setMenuRole(item.role);
    action->setCheckable(item.checkable);
    return action;
}

} // namespace

MenuRegistry::MenuRegistry(QObject* parent) : QObject(parent) {}

MenuRegistry::~MenuRegistry() = default;

void MenuRegistry::addItem(MenuItemSpec spec) {
    if (!spec.id.empty()) {
        const bool used = std::ranges::any_of(
            items_, [&](const MenuItemSpec& item) { return item.id == spec.id; });
        if (used) {
            throw std::invalid_argument(
                std::format("A menu item with the id '{}' is already registered.", spec.id));
        }
    }
    items_.push_back(std::move(spec));
}

void MenuRegistry::addSeparator(Menu menu, int order) {
    items_.push_back(MenuItemSpec{menu, order, {}});
}

std::vector<MenuItemSpec> MenuRegistry::items(Menu menu) const {
    std::vector<MenuItemSpec> found;
    for (const auto& item : items_) {
        if (item.menu == menu) {
            found.push_back(item);
        }
    }
    std::ranges::stable_sort(found, {}, &MenuItemSpec::order);
    return found;
}

QAction* MenuRegistry::add(const MenuItem& item) {
    if (item.id.empty()) {
        throw std::invalid_argument("A menu item needs an id.");
    }
    const bool used =
        std::ranges::any_of(items_, [&](const MenuItemSpec& spec) { return spec.id == item.id; });
    if (used) {
        throw std::invalid_argument(
            std::format("A menu item with the id '{}' is already registered.", item.id));
    }
    QAction* action = build_action(item, this);
    if (item.separatorBefore) {
        addSeparator(item.menu, item.order);
    }
    QPointer<QAction> kept(action);
    items_.push_back(MenuItemSpec{item.menu, item.order,
                                  [kept](QObject*) -> QAction* { return kept.data(); }, item.id});
    actions_[item.id] = action;
    return action;
}

QAction* MenuRegistry::action(std::string_view id) const {
    const auto it = actions_.find(id);
    return it == actions_.end() ? nullptr : it->second.data();
}

void MenuRegistry::populate(Menu which, QMenu* menu) {
    if (menu == nullptr) {
        return;
    }
    for (const auto& item : items(which)) {
        if (!item.create) {
            menu->addSeparator();
            continue;
        }
        QAction* action = item.create(menu);
        if (action == nullptr) {
            continue;
        }
        menu->addAction(action);
        if (!item.id.empty()) {
            actions_[item.id] = action;
        }
    }
}

} // namespace fv
