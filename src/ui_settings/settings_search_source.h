#pragma once

#include <optional>
#include <vector>

#include "appcore/settings_search.h"

namespace fv {

// A Settings page that lists search results of its own once it is built: the Downloads page's
// model and component rows, with their sizes and badges (settings_search.py _catalog_entries,
// its branch for a built Downloads page). The search palette asks the built page's content
// widget while it builds its index.
// Affinity: GUI thread.
class SettingsSearchSource {
public:
    virtual ~SettingsSearchSource() = default;

    // The page's entries; nullopt while the page's widgets are not built, and the palette then
    // lists the catalogs instead.
    virtual std::optional<std::vector<appcore::SearchEntry>> searchEntries() = 0;

protected:
    SettingsSearchSource() = default;
    SettingsSearchSource(const SettingsSearchSource&) = default;
    SettingsSearchSource& operator=(const SettingsSearchSource&) = default;
};

} // namespace fv
