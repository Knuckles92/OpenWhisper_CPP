#include <algorithm>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>

#include "appkit/destinations.h"
#include "appkit/registries.h"

namespace fv {

// Owns the key the spec's destination views.
struct SettingsPageRegistry::Entry {
    std::string key;
    SettingsPageSpec spec;
};

SettingsPageRegistry::SettingsPageRegistry() = default;

SettingsPageRegistry::~SettingsPageRegistry() = default;

void SettingsPageRegistry::addPage(SettingsPageSpec spec) {
    const std::string_view key = destinations::resolve_destination(spec.destination);
    if (key.empty()) {
        throw std::invalid_argument("A Settings page needs a destination.");
    }
    if (page(key) != nullptr) {
        throw std::invalid_argument(
            std::format("A Settings page for '{}' is already registered.", key));
    }
    auto entry = std::make_unique<Entry>(Entry{std::string(key), std::move(spec)});
    entry->spec.destination = entry->key;
    entries_.push_back(std::move(entry));
}

const SettingsPageSpec* SettingsPageRegistry::page(std::string_view destination) const {
    const std::string_view key = destinations::resolve_destination(destination);
    for (const auto& entry : entries_) {
        if (entry->key == key) {
            return &entry->spec;
        }
    }
    return nullptr;
}

std::vector<const SettingsPageSpec*> SettingsPageRegistry::pages() const {
    std::vector<const SettingsPageSpec*> ordered;
    std::vector<std::string_view> listed;
    const auto take = [&](std::string_view key) {
        listed.push_back(key);
        if (const SettingsPageSpec* spec = page(key)) {
            ordered.push_back(spec);
        }
    };
    take(destinations::home.key);
    for (const auto& chapter : destinations::rail) {
        for (const auto& rail_page : chapter.pages) {
            take(rail_page.key);
        }
    }
    for (const auto& entry : entries_) {
        if (std::ranges::find(listed, std::string_view(entry->key)) == listed.end()) {
            ordered.push_back(&entry->spec);
        }
    }
    return ordered;
}

std::string_view SettingsPageRegistry::controlDestination(std::string_view control) const {
    // Newest first: OpenWhisper's merge_page_module overwrites CONTROL_DESTINATIONS, so the
    // page registered last with a name wins.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        const auto& controls = (*it)->spec.controls;
        if (std::ranges::find(controls, control) != controls.end()) {
            return (*it)->key;
        }
    }
    return {};
}

} // namespace fv
