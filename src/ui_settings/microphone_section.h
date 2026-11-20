#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "appcore/mic_priority.h"
#include "appkit/qt_bridge.h"
#include "audio/backend.h"
#include "base/json.h"
#include "ui_settings/json_metatype.h"

class QBoxLayout;
class QFrame;
class QPushButton;
class QVBoxLayout;

namespace fv {

class Button;
class ElidingComboBox;
class Executor;
class FieldTile;
class InfoTile;
class SettingsDialog;
class SettingsPage;
class WrappedLabel;

namespace audio {
struct InputDevice;
}
namespace detail {
template <class Widget> class Mailbox;
}

// The microphone combo with Refresh beside it, or under it when the row is narrower than
// kNarrowBelow at 100 % text (settings_microphones.py _PreferredRow, "micPreferredRow").
// Affinity: GUI thread.
class PreferredRow : public QWidget {
    Q_OBJECT
public:
    // Room for a typical "Microphone (USB Audio Device)" plus the button.
    static constexpr int kNarrowBelow = 400;

    PreferredRow(QWidget* combo, QWidget* button, QWidget* parent = nullptr);
    QBoxLayout* boxLayout() const { return layout_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* button_;
    QBoxLayout* layout_;
};

// Settings → Recording → Microphone (ui_qt/dialogs/settings_microphones.py MicrophoneSection):
// the preferred microphone and its backups. audio_input_priority is the single source of truth
// (appcore::mic decides what each control shows); the section lists the devices off the GUI
// thread, with a generation, because the driver can take seconds, and nothing is called "Not
// connected" before the first list arrives. Every rebuild keeps keyboard focus on the same row's
// button, or its neighbour, and links the rows into the Tab order between Refresh and the Add
// picker.
// Affinity: GUI thread.
class MicrophoneSection : public QObject {
    Q_OBJECT
public:
    struct Ports {
        // AudioRecorder.get_input_devices; blocking, on `worker`.
        std::function<std::vector<audio::InputDevice>()> listDevices;
        // audio_devices.refresh_portaudio: false when refused; throws when the audio system does
        // not restart. Blocking, on `worker`.
        std::function<bool()> refreshDevices;
        std::function<bool()> meetingActive;
        std::function<bool()> playbackPlaying;
        Executor* worker = nullptr;
    };

    MicrophoneSection(SettingsDialog& dialog, Ports ports, QObject* parent = nullptr);
    ~MicrophoneSection() override;

    // Adds the "Microphone" group (the Preferred microphone and "If it's unplugged, use" tiles)
    // to layout, shows the saved order, and starts listing devices.
    void build(SettingsPage& page, QVBoxLayout* layout);
    void load(const Json& settings);
    // Lists the devices on the worker; with refresh, re-reads the audio system first.
    void enumerate(bool refresh = false);
    // A device list from the worker; outcome is kOutcomeRefreshed, kOutcomeBusy or "".
    void applyDevices(std::vector<audio::InputDevice> devices, const QString& outcome);
    // Refresh: refused ("Microphones are in use. ...") while a stream is open, a meeting runs or
    // playback plays; otherwise re-reads the devices, the button disabled meanwhile.
    void refreshDevices();
    // The preferred combo's choice (a token, or "" for System default).
    void choosePreferred(const QString& token);

    ElidingComboBox* combo() const { return combo_; }
    FieldTile* preferredTile() const { return preferredTile_; }
    InfoTile* backupTile() const { return backupTile_; }
    Button* refreshButton() const { return refreshButton_; }
    WrappedLabel* status() const { return status_; }
    QWidget* rows() const { return rows_; }
    QVBoxLayout* rowsLayout() const { return rowsLayout_; }
    ElidingComboBox* addCombo() const { return addCombo_; }
    // nullopt until the first list arrives.
    const appcore::mic::Devices& devices() const { return devices_; }
    const std::vector<audio::DeviceKey>& priority() const { return priority_; }
    // A backup row's button of the kind "up", "down" or "remove"; null when the row has none.
    static QPushButton* rowButton(QWidget* row, const QString& kind);

Q_SIGNALS:
    // on_audio_device_changed: the new order, once saved.
    void audioDeviceChanged(const fv::Json& priority);

private:
    struct Held {
        QString kind;
        QString token;
        int index = -1;
    };

    void move(std::size_t index, int offset);
    void remove(std::size_t index);
    void onAddChosen();
    void save(std::vector<audio::DeviceKey> priority, std::optional<Json> updates = std::nullopt,
              std::vector<std::string> drops = {});
    void say(const QString& text);
    void render();
    std::optional<Held> heldFocus() const;
    QWidget* focusTarget(const Held& held) const;
    void fillPreferred();
    std::vector<QWidget*> fillRows();
    QFrame* row(const QString& label, const QString& note, const QString& state,
                const QString& entry = {}, std::function<void()> up = {},
                std::function<void()> down = {}, std::function<void()> remove = {});
    void fillAddCombo();
    QString tokenOf(const QWidget* button) const;

    SettingsDialog& dialog_;
    Ports ports_;
    std::shared_ptr<detail::Mailbox<MicrophoneSection>> mailbox_;
    // _audio_device_generation: only the newest list is applied.
    qt::TokenGate generation_;

    ElidingComboBox* combo_ = nullptr;
    Button* refreshButton_ = nullptr;
    FieldTile* preferredTile_ = nullptr;
    WrappedLabel* status_ = nullptr;
    InfoTile* backupTile_ = nullptr;
    QWidget* rows_ = nullptr;
    QVBoxLayout* rowsLayout_ = nullptr;
    ElidingComboBox* addCombo_ = nullptr;

    appcore::mic::Devices devices_;
    std::vector<audio::DeviceKey> priority_;
    // Each backup row's entry token (Python keeps it as the buttons' "entry" property).
    std::vector<std::pair<QPointer<QWidget>, QString>> rowTokens_;
};

} // namespace fv
