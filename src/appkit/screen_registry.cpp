#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <utility>

#include <QWidget>

#include "appkit/qt_bridge.h"
#include "appkit/registries.h"

namespace fv {

namespace {

// One kept call. Python keys update_screen's cache by method, or by (method, first argument)
// for set_model_downloading.
struct Update {
    std::string method;
    std::string coalesce;
    ScreenRegistry::Apply apply;
    std::optional<Json> merged; // the kept dict of an updateMerged call
};

using Updates = std::vector<Update>;

Update* find_update(Updates& updates, std::string_view method, std::string_view coalesce) {
    const auto it = std::ranges::find_if(updates, [&](const Update& update) {
        return update.method == method && update.coalesce == coalesce;
    });
    return it == updates.end() ? nullptr : &*it;
}

// dict.pop(key) then dict[key] = value: the newest call goes to the end of the replay order.
void keep_latest(Updates& updates, Update update) {
    std::erase_if(updates, [&](const Update& kept) {
        return kept.method == update.method && kept.coalesce == update.coalesce;
    });
    updates.push_back(std::move(update));
}

std::vector<ScreenSpec> sorted(std::vector<ScreenSpec> specs) {
    std::ranges::stable_sort(specs, {}, &ScreenSpec::order);
    return specs;
}

} // namespace

struct ScreenRegistry::Impl {
    struct Screen {
        ScreenSpec spec;
        QPointer<QWidget> widget;
    };

    std::vector<ScreenSpec> tabs;
    std::vector<ScreenSpec> side_panels;
    // Keyed screens, tabs and side panels alike.
    std::map<std::string, Screen, std::less<>> screens;
    std::map<std::string, Updates, std::less<>> updates;
    std::map<std::string, Updates, std::less<>> group_updates;
    Placer placer;

    void add(std::vector<ScreenSpec>& list, ScreenSpec spec) {
        if (!spec.key.empty()) {
            if (screens.contains(spec.key)) {
                throw std::invalid_argument(
                    std::format("A screen with the key '{}' is already defined.", spec.key));
            }
            screens.emplace(spec.key, Screen{spec, {}});
        }
        list.push_back(std::move(spec));
    }

    QWidget* existing(std::string_view key) const {
        const auto it = screens.find(key);
        return it == screens.end() ? nullptr : it->second.widget.data();
    }

    // The group's existing screens, by order then key.
    std::vector<QWidget*> members(std::string_view group) const {
        std::vector<const Screen*> found;
        for (const auto& [key, screen] : screens) {
            if (screen.spec.group == group && !screen.widget.isNull()) {
                found.push_back(&screen);
            }
        }
        std::ranges::stable_sort(found, {},
                                 [](const Screen* screen) { return screen->spec.order; });
        std::vector<QWidget*> widgets;
        widgets.reserve(found.size());
        for (const Screen* screen : found) {
            widgets.push_back(screen->widget.data());
        }
        return widgets;
    }
};

ScreenRegistry::ScreenRegistry(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

ScreenRegistry::~ScreenRegistry() = default;

void ScreenRegistry::addScreen(ScreenSpec spec) {
    impl_->add(impl_->tabs, std::move(spec));
}

void ScreenRegistry::addSidePanel(ScreenSpec spec) {
    impl_->add(impl_->side_panels, std::move(spec));
}

std::vector<ScreenSpec> ScreenRegistry::screens() const {
    return sorted(impl_->tabs);
}

std::vector<ScreenSpec> ScreenRegistry::sidePanels() const {
    return sorted(impl_->side_panels);
}

void ScreenRegistry::define(std::string key, int order, QString title, Factory factory,
                            std::string group) {
    if (key.empty()) {
        throw std::invalid_argument("A screen needs a key to be defined.");
    }
    addScreen(
        ScreenSpec{order, std::move(title), std::move(factory), std::move(key), std::move(group)});
}

bool ScreenRegistry::isDefined(std::string_view key) const {
    return impl_->screens.contains(key);
}

void ScreenRegistry::setPlacer(Placer placer) {
    impl_->placer = std::move(placer);
}

QWidget* ScreenRegistry::ensure(std::string_view key, QWidget* parent) {
    const auto it = impl_->screens.find(key);
    if (it == impl_->screens.end()) {
        throw std::invalid_argument(std::format("No screen is defined with the key '{}'.", key));
    }
    if (QWidget* widget = it->second.widget.data()) {
        return widget;
    }
    const ScreenSpec spec = it->second.spec;
    QWidget* widget = spec.create ? spec.create(parent) : nullptr;
    if (widget == nullptr) {
        throw std::logic_error(
            std::format("The screen '{}' was not created: its factory returned nothing.", key));
    }
    it->second.widget = widget;
    if (impl_->placer) {
        impl_->placer(spec.key, widget);
    }
    // Copies: a replayed setter may queue another update for this screen.
    if (!spec.group.empty()) {
        if (const auto group = impl_->group_updates.find(spec.group);
            group != impl_->group_updates.end()) {
            const Updates replay = group->second;
            for (const Update& update : replay) {
                update.apply(widget);
            }
        }
    }
    if (const auto own = impl_->updates.find(key); own != impl_->updates.end()) {
        const Updates replay = own->second;
        for (const Update& update : replay) {
            update.apply(widget);
        }
    }
    Q_EMIT screenCreated(qt::toQString(spec.key), widget);
    return widget;
}

QWidget* ScreenRegistry::existing(std::string_view key) const {
    return impl_->existing(key);
}

void ScreenRegistry::update(std::string_view key, std::string method, Apply apply,
                            std::string coalesce) {
    auto& updates = impl_->updates[std::string(key)];
    keep_latest(updates, Update{std::move(method), std::move(coalesce), apply, std::nullopt});
    if (QWidget* widget = impl_->existing(key)) {
        apply(widget);
    }
}

void ScreenRegistry::updateMerged(std::string_view key, std::string method, const Json& patch,
                                  MergedApply apply) {
    if (!patch.is_object()) {
        throw std::invalid_argument(
            std::format("The update {} of screen '{}' merges an object, not {}.", method, key,
                        patch.type_name()));
    }
    auto& updates = impl_->updates[std::string(key)];
    Json merged = Json::object();
    if (const Update* previous = find_update(updates, method, {});
        previous != nullptr && previous->merged) {
        merged = *previous->merged;
    }
    for (const auto& [name, value] : patch.items()) {
        merged[name] = value;
    }
    Apply bound = [apply, merged](QWidget* widget) {
        apply(widget, merged);
    };
    keep_latest(updates, Update{std::move(method), {}, bound, merged});
    if (QWidget* widget = impl_->existing(key)) {
        bound(widget);
    }
}

const Json* ScreenRegistry::mergedState(std::string_view key, std::string_view method) const {
    const auto it = impl_->updates.find(key);
    if (it == impl_->updates.end()) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(it->second, [&](const Update& update) {
        return update.method == method && update.coalesce.empty();
    });
    if (found == it->second.end() || !found->merged) {
        return nullptr;
    }
    return &*found->merged;
}

void ScreenRegistry::updateGroup(std::string_view group, std::string method, Apply apply) {
    keep_latest(impl_->group_updates[std::string(group)],
                Update{std::move(method), {}, apply, std::nullopt});
    for (QWidget* widget : impl_->members(group)) {
        apply(widget);
    }
}

void ScreenRegistry::syncGroup(std::string_view group, std::string method, Apply apply,
                               const QWidget* source) {
    auto& updates = impl_->group_updates[std::string(group)];
    // dict[key] = value without the pop: an existing entry keeps its place.
    if (Update* kept = find_update(updates, method, {})) {
        kept->apply = apply;
    } else {
        updates.push_back(Update{std::move(method), {}, apply, std::nullopt});
    }
    for (QWidget* widget : impl_->members(group)) {
        if (widget != source) {
            apply(widget);
        }
    }
}

std::vector<QWidget*> ScreenRegistry::existingInGroup(std::string_view group) const {
    return impl_->members(group);
}

} // namespace fv
