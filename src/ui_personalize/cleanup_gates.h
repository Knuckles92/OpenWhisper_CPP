#pragma once

#include <functional>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"

class QWidget;

namespace fv {

class Button;
class InfoTile;
class SettingsStore;

// The "AI cleanup is off" notices of the Personalize pages and their one-click "Turn on"
// (settings_dialog.py cleanup_gate_tile, refresh_cleanup_gates, turn_on_cleanup and
// _update_cleanup_gates). Pages whose settings only matter while cleanup runs put one above
// them. Provided once for the app with ctx.provide<CleanupGates>() by the Personalize feature;
// the tiles of every Settings window register here, and one that is gone is forgotten.
// Affinity: GUI thread.
class CleanupGates : public QObject {
    Q_OBJECT
public:
    explicit CleanupGates(SettingsStore& settings, QObject* parent = nullptr);
    ~CleanupGates() override;

    // An "AI cleanup is off" notice whose button turns cleanup on. It is hidden while cleanup
    // is on; `needed`, when given, must also answer true for it to show (call refresh when its
    // answer may have changed). The tile is shown only once it has a parent: a tile shown
    // before its page adopts it would open as a window of its own.
    InfoTile* gateTile(const QString& description, std::function<bool()> needed = {},
                       QWidget* parent = nullptr);
    // The tile's "Turn on (<level>)" button; null for a tile that is not a gate.
    Button* turnOnButton(const InfoTile* tile) const;

    // Shows or hides every notice for a settings snapshot, and names the level on its button.
    void refresh(const Json& settings);
    // Turns AI cleanup on at its saved level. Without a handler (the AI cleanup page installs
    // one), it saves transcript_cleanup_enabled = true through the Settings window `origin` sits
    // in (through the store when it sits in none), says "cleanup" changed and refreshes.
    void turnOnCleanup(QWidget* origin = nullptr);
    void setTurnOnHandler(std::function<void()> handler);
    // The same, told the widget the turn-on came from (the Settings window to save through).
    void setTurnOnHandlerWithOrigin(std::function<void(QWidget* origin)> handler);

    // "Turn on (<Light, Medium, High or Custom>)": the level cleanup would run at.
    static QString turnOnLabel(const Json& settings);

private:
    struct Gate {
        QPointer<InfoTile> tile;
        QPointer<Button> button;
        std::function<bool()> needed;
    };

    SettingsStore& settings_;
    std::vector<Gate> gates_;
    std::function<void(QWidget*)> handler_;
};

} // namespace fv
