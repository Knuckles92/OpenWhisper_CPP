#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <QVariant>

#include "base/json.h"

class QAbstractButton;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace fv {

// A settings value as a combo item's data, and back: strings, booleans, integers and floats keep
// their type; null is an invalid QVariant. Anything else (a list, an object) goes through its
// JSON text.
QVariant jsonToVariant(const Json& value);
Json variantToJson(const QVariant& value);

// Two-way links between settings keys and the simple Settings controls
// (ui_qt/dialogs/settings_binder.py). A bound control saves its value through
// persist(key, value) the moment it changes, and load() puts every bound control back to the
// value its settings resolve to. persist owns the loading guard: the dialog's persist ignores
// changes made while it loads, so the signals load() triggers never write anything back.
//
// Only controls whose change does nothing but save belong here. A control that also restyles
// the page, asks first, or calls back into the app keeps its own handler. The binder does not
// own the controls: a destroyed control no longer saves, and load() skips it.
// Affinity: GUI thread.
class SettingsBinder {
public:
    using Persist = std::function<bool(std::string_view key, const Json& value)>;
    // The value a control shows for a settings snapshot.
    using Resolve = std::function<Json(const Json& settings)>;
    using Apply = std::function<void(const Json& value)>;

    explicit SettingsBinder(Persist persist);
    SettingsBinder(const SettingsBinder&) = delete;
    SettingsBinder& operator=(const SettingsBinder&) = delete;

    // Saves bool(checked) on toggle; loads resolve(settings) by Python truthiness.
    void checkbox(QAbstractButton* check, std::string key, Resolve resolve);
    // Saves the current item's data; loads the item whose data matches, else the first item.
    void combo(QComboBox* combo, std::string key, Resolve resolve);
    // Saves int(value) on every committed value; loads resolve(settings).
    void spin(QSpinBox* spin, std::string key, Resolve resolve);
    // Saves the trimmed text once editing finishes, not on every keystroke: half-typed text
    // never reaches the file.
    void lineEdit(QLineEdit* edit, std::string key, Resolve resolve);
    // Saves the trimmed text once the editor loses focus, and only an edit: passing focus through
    // would put back what the editor showed when it loaded, over a change made elsewhere since.
    // Text that only repeats the placeholder, which shows the default, saves as "", so the
    // default stays current when it changes.
    void plainText(QPlainTextEdit* edit, std::string key, Resolve resolve);
    // A binding whose save the caller connects itself: load() calls apply(resolve(settings)).
    void custom(Resolve resolve, Apply apply);

    // Controls registered so far, so a page built later loads only its own bindings.
    int bindingCount() const { return static_cast<int>(bindings_.size()); }
    // Sets the bound controls from settings in registration order, beginning at start.
    void load(const Json& settings, int start = 0) const;

private:
    struct Binding {
        Resolve resolve;
        Apply apply;
    };

    Persist persist_;
    std::vector<Binding> bindings_;
};

} // namespace fv
