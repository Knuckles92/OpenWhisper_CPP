#pragma once

#include <string_view>

#include "base/json.h"

class QWidget;

namespace fv {

// The hooks a built Settings page answers (settings_dialog.py's page-module protocol: load,
// refresh, cancel_capture, save_drafts and the control attributes the dialog exposes). A page
// widget that needs them also derives from this; the shell finds it with dynamic_cast on the
// widget its SettingsPageSpec::create returned and calls the hooks only after the page is built.
// Every hook has a default, so a page implements only what it uses. The shell catches what a
// hook throws, so one page's failure cannot stop Settings opening, switching or closing.
// Affinity: UI actor.
class SettingsPageModule {
public:
    virtual ~SettingsPageModule() = default;

    // Shows the saved values; the shell runs it with its loading flag set, so nothing the page
    // does in it is saved.
    virtual void loadSettings(const Json& settings) { (void)settings; }
    // Re-reads state that is not in the settings snapshot (a list, a scan).
    virtual void refreshPage() {}
    // Stops a shortcut capture the page started (switching pages, closing).
    virtual void cancelCapture() {}
    // Saves the page's unsaved drafts. With `ask` the page may ask first; false keeps Settings
    // on this page (a draft that cannot be saved yet, and the user chose to keep editing).
    virtual bool saveDrafts(bool ask) {
        (void)ask;
        return true;
    }
    // The page was selected and shown.
    virtual void onShown() {}
    // The control a deep link, a search result or a test names (one of the spec's `controls` or
    // a search field's `target`); null when the page has no such control.
    virtual QWidget* namedControl(std::string_view name) {
        (void)name;
        return nullptr;
    }

protected:
    SettingsPageModule() = default;
    SettingsPageModule(const SettingsPageModule&) = default;
    SettingsPageModule& operator=(const SettingsPageModule&) = default;
};

} // namespace fv
