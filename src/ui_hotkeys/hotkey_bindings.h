#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QString>
#include <QWidget>

#include "base/json.h"
#include "hotkeys/conflicts.h"

class QVBoxLayout;

namespace fv {

class SettingsStore;
class WrappedLabel;

// The Hotkeys page's rows for the shortcuts people give their own profiles, transforms and
// actions (ui_qt/widgets/hotkey_bindings.py). Their own editors set those shortcuts too; a row
// here saves through the same validation, so a shortcut is refused here exactly when it would be
// there.
namespace hotkey_bindings {

// Binding: one profile, transform or action, as its row shows it.
struct Binding {
    std::string kind; // hotkeys::kProfile, kTransform or kAction
    std::string id;
    std::string name;
    std::string hotkey;
    std::string detail;
    bool enabled = true;

    friend bool operator==(const Binding&, const Binding&) = default;
};

// _Group: a kind's heading, its own page and what its rows say.
struct Group {
    std::string_view kind;
    std::string_view title;
    std::string_view destination; // appkit/destinations.h
    std::string_view link;
    std::string_view caption; // what pressing one of the group's shortcuts does
    std::string_view empty;   // shown instead of rows when the kind has none
};

// GROUPS, in page order.
const std::array<Group, 3>& groups();

// A profile's or transform's instructions are quoted up to this many characters.
inline constexpr std::size_t kInstructionChars = 80;

// _clip: whitespace runs joined by one space; longer than kInstructionChars code points, the
// first kInstructionChars - 1, right-stripped, and "…".
std::string clip(std::string_view text);

} // namespace hotkey_bindings

// Where one kind of binding lives. The module that owns profiles, transforms or actions
// registers one (M4 and M5); a kind without a source has no rows.
// Affinity: called on the GUI thread; save() runs inside SettingsStore::mutate.
class BindingSource {
public:
    virtual ~BindingSource() = default;

    // The kind's bindings in their saved order, read from a settings snapshot (bindings()).
    virtual std::vector<hotkey_bindings::Binding> list(const Json& settings) const = 0;
    // Gives binding `id` the shortcut `hotkey` ("" clears it) in `settings`, through the same
    // validation its own editor uses. False when the binding no longer exists; a refusal throws
    // py::ValueError with the reason its editor gives ("That shortcut is already used by
    // Cancel. Choose another."); anything else thrown means the settings could not be written.
    virtual bool save(Json& settings, const std::string& id, const std::string& hotkey) = 0;
    // After a save through a row, with the settings written: what the kind's own page and the
    // app need to hear (Settings' _on_binding_shortcut_saved).
    virtual void saved() {}

protected:
    BindingSource() = default;
    BindingSource(const BindingSource&) = default;
    BindingSource& operator=(const BindingSource&) = default;
};

// The registered sources by kind.
using BindingSources = std::map<std::string, std::shared_ptr<BindingSource>, std::less<>>;

// Every registered binding as hotkey_conflict compares them.
hotkeys::Bindings conflictBindings(const BindingSources& sources, const Json& settings);

// BindingShortcuts: profiles, transforms and actions, each with its shortcut to change or clear.
// Affinity: GUI thread.
class BindingShortcuts : public QWidget {
    Q_OBJECT

public:
    explicit BindingShortcuts(SettingsStore& store, QWidget* parent = nullptr);
    ~BindingShortcuts() override;

    // Replaces the kind's source; the rows follow on the next load.
    void registerSource(std::string kind, std::shared_ptr<BindingSource> source);
    const BindingSources& sources() const { return sources_; }

    // Whether a row's field is waiting for keys.
    bool capturing() const;
    void cancelCapture();
    // Shows what is saved; a field waiting for keys keeps waiting (nothing reloads then).
    void load(const Json& settings);

Q_SIGNALS:
    // A row's field started (true) or stopped (false) waiting for keys.
    void captureChanged(bool capturing);
    // A binding of `kind` got a new shortcut; message says so.
    void saved(const QString& kind, const QString& message);
    // A group's link: the Settings destination to show.
    void openRequested(const QString& destination);

protected:
    void showEvent(QShowEvent* event) override;

private:
    class Row;
    struct GroupWidgets {
        QVBoxLayout* rows = nullptr;
        WrappedLabel* caption = nullptr;
        WrappedLabel* empty = nullptr;
        std::vector<Row*> items;
    };

    // bindings(settings): every registered kind's bindings, in GROUPS order.
    std::vector<std::pair<std::string, std::vector<hotkey_bindings::Binding>>>
    bindings(const Json& settings) const;
    void rebuild(const std::string& kind, const std::vector<hotkey_bindings::Binding>& items);
    void save(Row* row, const QString& hotkey);

    SettingsStore& store_;
    BindingSources sources_;
    std::map<std::string, GroupWidgets, std::less<>> groups_;
};

} // namespace fv
