#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>
#include <QString>
#include <QUrl>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "appkit/settings_page.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/subscription.h"
#include "remote/control.h"
#include "remote/record_sync_control.h"
#include "speech/service.h"

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace fv {

class Button;
class FieldTile;
class InfoTile;
class NoWheelSpinBox;
class SegmentedBar;
class SettingTile;
class SettingsPage;
class WrappedLabel;

// The dialogs the page asks through; each defaults to the real one, and tests answer them.
struct RemoteEnginePrompts {
    enum class RemoveChoice { Cancel, KeepRecords, DeleteRecords };

    // _ask_name: the name typed in ("" goes back to `own`), or nullopt when canceled.
    std::function<std::optional<QString>(QWidget* parent, const QString& title, const QString& text,
                                         const QString& current, const QString& own)>
        askName;
    // A Yes/No question (QMessageBox.question); true for Yes.
    std::function<bool(QWidget* parent, const QString& title, const QString& text)> question;
    std::function<void(QWidget* parent, const QString& title, const QString& text)> warning;
    std::function<void(QWidget* parent, const QString& title, const QString& text)> information;
    // "Remove paired computer" for a computer that keeps `stored` here.
    std::function<RemoveChoice(QWidget* parent, const QString& name, const QString& stored)>
        removeDevice;
    // The recovery dialog: (owner id, device id), or nullopt when canceled.
    std::function<std::optional<std::pair<std::string, std::string>>(
        QWidget* parent, const std::vector<remote::RecordOwnerGroup>& owners,
        const std::vector<remote::PairedDevice>& devices)>
        recoverRecords;
};

// What the page works with. A null service is the unbound page (A18-46); a null record sync
// hides the storage tile. Everything that may block (every service call but the cheap readers,
// every record-sync call but location()) runs on `io`; results come back to the page on its own
// thread, each job kind behind its own TokenGate, and are dropped once the page is gone.
struct RemoteEnginePageServices {
    remote::RemoteEngineControl* service = nullptr;
    remote::RecordSyncControl* records = nullptr;
    // Required with a service; must outlive the page's jobs, as the service and records must.
    Executor* io = nullptr;
    // steady() times the search cache; zone() shows pairing dates. system_clock() when null.
    const Clock* clock = nullptr;
    // The Settings rail's value for this page (A18-02).
    std::function<void(const QString&)> setRailValue;
    // Opens a note's fix, the Tailscale download page; QDesktopServices::openUrl when unset.
    std::function<void(const QUrl&)> openUrl;
    RemoteEnginePrompts prompts;
    speech::SpeechService* speech = nullptr;
    std::function<bool()> usesRemoteDictation;
    std::function<void()> retryRemoteDictation;
    bool canBrowseHeldMeetings = false;
    bool canPublishHistory = false;
    bool canGrantMcpSession = false;
};

// Settings → Remote engine (ui_qt/dialogs/settings_remote.py RemoteEngineSection): use another
// computer's engine (find, pair, use, manage, rename, forget, where records go) or share this
// one (sharing, reachability notes, permissions, Tailscale trust, port, pairing code, paired
// computers, record recovery), one tab each. The page body: its column holds the tab bar and the
// two sections, as OpenWhisper's page layout does.
//
// State is read on io into a snapshot (host_state, the pairing, the record sync's status, each
// paired computer's records) and drawn from it; refresh() draws the last snapshot at once and
// asks for a new one. Page-local state (what is busy, the open address row, the request waiting
// on the host) draws at once.
// Affinity: GUI thread.
class RemoteEnginePage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // `page` lays out the tile grids (they reflow with its width); the page body's parent is
    // `parent`, which may be page itself.
    RemoteEnginePage(SettingsPage& page, RemoteEnginePageServices services,
                     QWidget* parent = nullptr);
    ~RemoteEnginePage() override;

    // Draws what is known and reads the state again.
    void refresh();
    // The page came into view: ask the host what it keeps, or look for hosts.
    void onShown() override;
    // Look for computers sharing their engine, here and on the tailnet. A search in the last
    // TAILNET_RESCAN_S seconds is reused unless forced.
    void scanNearby(bool force = false, bool sweep = false);

    // SettingsPageModule.
    void refreshPage() override { refresh(); }
    // "remote_publish_history": "Share history for this session…", the one control that opens
    // a history session (a17-direct-history); null when no publication is provided.
    QWidget* namedControl(std::string_view name) override;

    // _pair_error: the message a failed pairing shows; `host` is the found computer it was for.
    static QString pairErrorText(std::exception_ptr error,
                                 const std::optional<remote::NearbyHost>& host);

    // ---- what tests and the dashboard read ----
    SegmentedBar* tabs() const { return tabs_; }
    QWidget* clientContainer() const { return clientContainer_; }
    QWidget* hostContainer() const { return hostContainer_; }
    InfoTile* findTile() const { return findTile_; }
    QWidget* findList() const { return findList_; }
    QWidget* waitBox() const { return waitBox_; }
    QLabel* waitCodeLabel() const { return waitCodeLabel_; }
    WrappedLabel* waitLabel() const { return waitLabel_; }
    Button* waitCancelButton() const { return waitCancelButton_; }
    Button* searchButton() const { return searchButton_; }
    Button* sweepButton() const { return sweepButton_; }
    Button* manualButton() const { return manualButton_; }
    InfoTile* clientTile() const { return clientTile_; }
    QWidget* pairRow() const { return pairRow_; }
    QLineEdit* addressEdit() const { return addressEdit_; }
    QLineEdit* codeEdit() const { return codeEdit_; }
    Button* pairButton() const { return pairButton_; }
    WrappedLabel* findMessage() const { return findMessage_; }
    QWidget* credentialRecovery() const { return credentialRecovery_; }
    WrappedLabel* credentialHelp() const { return credentialHelp_; }
    Button* retryKeyButton() const { return retryKeyButton_; }
    Button* openKeychainButton() const { return openKeychainButton_; }
    QWidget* pairedRow() const { return pairedRow_; }
    Button* useButton() const { return useButton_; }
    Button* meetingUseButton() const { return meetingUseButton_; }
    Button* manageButton() const { return manageButton_; }
    Button* renameHostButton() const { return renameHostButton_; }
    Button* forgetButton() const { return forgetButton_; }
    WrappedLabel* clientMessage() const { return clientMessage_; }
    WrappedLabel* tailscaleHint() const { return tailscaleHint_; }
    InfoTile* storageTile() const { return storageTile_; }
    QPushButton* locationButton(const std::string& location) const;
    WrappedLabel* storageStatus() const { return storageStatus_; }
    Button* sendExistingButton() const { return sendExistingButton_; }
    Button* bringBackButton() const { return bringBackButton_; }
    Button* retryRecordsButton() const { return retryRecordsButton_; }
    SettingTile* shareTile() const { return shareTile_; }
    WrappedLabel* hostStatus() const { return hostStatus_; }
    WrappedLabel* hostIdentity() const { return hostIdentity_; }
    QWidget* hostNotes() const { return hostNotes_; }
    SettingTile* tailscaleTile() const { return tailscaleTile_; }
    SettingTile* managementTile() const { return managementTile_; }
    SettingTile* keepRecordsTile() const { return keepRecordsTile_; }
    SettingTile* manageMcpTile() const { return manageMcpTile_; }
    NoWheelSpinBox* portSpin() const { return portSpin_; }
    FieldTile* portTile() const { return portTile_; }
    InfoTile* devicesTile() const { return devicesTile_; }
    QWidget* pairingBox() const { return pairingBox_; }
    QLabel* pairingCodeLabel() const { return pairingCodeLabel_; }
    WrappedLabel* pairingExpiryLabel() const { return pairingExpiryLabel_; }
    Button* cancelPairingButton() const { return cancelPairingButton_; }
    Button* pairDeviceButton() const { return pairDeviceButton_; }
    QWidget* devicesList() const { return devicesList_; }
    Button* recoverRecordsButton() const { return recoverRecordsButton_; }
    WrappedLabel* recoveryMessage() const { return recoveryMessage_; }

    bool pairingBusy() const { return pairingBusy_; }
    bool scanBusy() const { return scanBusy_; }
    bool recoveryBusy() const { return recoveryBusy_; }
    bool recordsBusy() const { return !recordsBusy_.isEmpty(); }
    // A search has finished (and did not fail); whether it also tried every address.
    bool hasScan() const { return scan_.has_value(); }
    bool scanSwept() const { return scan_ && scan_->swept; }
    // A state read is under way.
    bool refreshing() const { return pendingReads_ > 0; }
    // The rail value last drawn.
    QString railValue() const { return railValue_; }

Q_SIGNALS:
    // "Use for dictation": the controller selects REMOTE_ENGINE_DISPLAY.
    void useForDictationRequested();
    // "Use for meetings": the meeting voice page takes the remote engine, then
    // settingsDestinationRequested("meeting_voice") shows it.
    void useForMeetingsRequested();
    // "Manage host models": RemoteModelsDialog for the paired host.
    void publishHistoryRequested(const QString& host, const QString& fingerprint);
    void manageHostModelsRequested(const QString& host, const QString& fingerprint);
    void heldMeetingsRequested(const QString& host, const QString& fingerprint);
    void shareMeetingRequested(const QString& host, const QString& fingerprint);
    // A Settings destination to show.
    void settingsDestinationRequested(const QString& destination);

private:
    // What the page draws, read on io.
    struct Snapshot {
        std::optional<remote::ClientPairing> pairing;
        std::string pendingName;
        // Asked only while no search result is shown.
        std::optional<net::TailscaleStatus> tailscale;
        remote::HostState host;
        // nullopt: no record sync, or its status failed.
        std::optional<remote::SyncStatus> records;
        // records_summary of each paired computer, by id.
        std::map<std::string, Json> deviceRecords;
    };
    // While a pairing request waits on the host's owner.
    struct Request {
        QString name;
        QString sas;
        CancelSource cancel;
    };
    struct Job;

    void build(SettingsPage& page);
    void buildClient(SettingsPage& page);
    void buildHost(SettingsPage& page);

    // Runs work on io and hands its result (or what it threw) to done on this page, unless the
    // page is gone or, with latestOnly, gate was asked for a newer job meanwhile.
    template <class Work, class Done>
    void run(qt::TokenGate& gate, Work work, Done done, bool latestOnly = true);
    // The same for an action whose every result counts.
    template <class Work, class Done> void run(Work work, Done done);
    // The same without a gate (every one is delivered while the page lives).
    template <class Work> void runDetached(Work work);

    void requestSnapshot();
    // Drops the reads under way: they started before a change this page just made.
    void discardReads();
    void requestRecords();
    void render();
    void renderUnbound();
    void renderClient();
    void renderDictationLink();
    void renderRecords();
    void recordsStatusLines(const remote::SyncStatus& status, const QString& host,
                            const QString& stored, QStringList& lines) const;
    void showRecordActions(const remote::SyncStatus& status, const QString& host,
                           const QString& stored);
    void renderFind();
    QWidget* findRow(const remote::NearbyHost& host);
    void findNote(const QString& text, bool link = false);
    void clearFindRows();
    void renderHost();
    void syncHostSwitches(const remote::HostState& state);
    void showTailscaleTrust(const remote::HostState& state, bool onTailnet);
    void refreshTabs(const remote::HostState& state, bool running);
    void showSection(int index);
    void showHostNotes(const std::vector<remote::Note>& notes);
    void showPairing(const std::optional<std::pair<std::string, double>>& pairing);
    void refreshPairingCode();
    void rebuildDevices(const std::vector<remote::PairedDevice>& devices,
                        const std::vector<std::string>& connected);
    // Runs `then` on this page once io has answered that this computer is not paired.
    void whenUnpaired(std::function<void()> then);

    void say(const QString& text);
    void showManual();
    void pair();
    void startPairing(const QString& address, std::optional<QString> code, bool tailscale,
                      std::optional<remote::NearbyHost> host);
    void connectHost(const remote::NearbyHost& host);
    void retryPairingSave();
    void requestPairing(const remote::NearbyHost& host);
    void onPairCode(const QString& sas);
    void cancelRequest();
    void useCode(const remote::NearbyHost& host);
    void onPairFinished(std::optional<remote::ClientPairing> pairing, std::exception_ptr error,
                        std::optional<remote::NearbyHost> host);
    void useEngine();
    void manageModels();
    void useForMeetings();
    void renameHost();
    void forget();
    void onShareToggled(bool checked);
    void applySharing(std::function<void()> change);
    void setPermission(std::function<void(remote::RemoteEngineControl&)> change,
                       std::function<void(remote::HostState&)> shown);
    void chooseLocation(const std::string& location);
    void runRecordsJob(const QString& busy, std::function<QString()> work);
    void onRecordsEvent(const std::string& kind);
    void sendExisting();
    void bringBack();
    void onPortChanged();
    void applyPort();
    void openPairing();
    void cancelPairing();
    void renameDevice(const remote::PairedDevice& device);
    void removeDevice(const std::string& deviceId);
    void recoverRecords();
    void onServiceEvent(const std::string& kind);
    void openUrl(const QString& url);
    QString tailscaleLink(const QString& text) const;
    std::chrono::steady_clock::time_point now() const;

    RemoteEnginePageServices services_;
    remote::RemoteEngineControl* service_;
    remote::RecordSyncControl* records_;
    const Clock* clock_;
    // Posts to this page; jobs keep it, and what they post after the page is gone is dropped.
    std::shared_ptr<qt::QtUiExecutor> ui_;
    Subscription serviceSubscription_;
    Subscription recordsSubscription_;

    qt::TokenGate snapshotGate_;
    qt::TokenGate recordsGate_;
    qt::TokenGate countdownGate_;
    qt::TokenGate scanGate_;
    qt::TokenGate pairGate_;
    qt::TokenGate recordsJobGate_;
    qt::TokenGate recoveryGate_;
    qt::TokenGate locationGate_;
    qt::TokenGate unpairedGate_;
    qt::TokenGate actionGate_;
    int pendingReads_ = 0;

    Snapshot snapshot_;
    bool haveSnapshot_ = false;
    bool pairingBusy_ = false;
    bool scanBusy_ = false;
    std::optional<std::chrono::steady_clock::time_point> scanAt_;
    std::optional<remote::NearbyScan> scan_;
    bool sweeping_ = false;
    bool manual_ = false;
    std::optional<Request> request_;
    QString recordsBusy_;
    QString recordsNote_;
    bool recoveryBusy_ = false;
    bool tabChosen_ = false;
    std::optional<int> savedPort_;
    std::vector<std::vector<std::string>> hostNotesShown_;
    QString railValue_;

    SegmentedBar* tabs_ = nullptr;
    QWidget* clientContainer_ = nullptr;
    QWidget* hostContainer_ = nullptr;
    InfoTile* findTile_ = nullptr;
    QWidget* findList_ = nullptr;
    QVBoxLayout* findLayout_ = nullptr;
    QWidget* waitBox_ = nullptr;
    QLabel* waitCodeLabel_ = nullptr;
    WrappedLabel* waitLabel_ = nullptr;
    Button* waitCancelButton_ = nullptr;
    Button* publishHistory_ = nullptr;
    Button* searchButton_ = nullptr;
    Button* sweepButton_ = nullptr;
    Button* manualButton_ = nullptr;
    InfoTile* clientTile_ = nullptr;
    QWidget* pairRow_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QLineEdit* codeEdit_ = nullptr;
    Button* pairButton_ = nullptr;
    WrappedLabel* findMessage_ = nullptr;
    QWidget* credentialRecovery_ = nullptr;
    WrappedLabel* credentialHelp_ = nullptr;
    Button* retryKeyButton_ = nullptr;
    Button* retryDictation_ = nullptr;
    QLabel* dictationLink_ = nullptr;
    Subscription speechSubscription_;
    Button* openKeychainButton_ = nullptr;
    QWidget* pairedRow_ = nullptr;
    Button* useButton_ = nullptr;
    Button* meetingUseButton_ = nullptr;
    Button* manageButton_ = nullptr;
    Button* renameHostButton_ = nullptr;
    Button* forgetButton_ = nullptr;
    WrappedLabel* clientMessage_ = nullptr;
    WrappedLabel* tailscaleHint_ = nullptr;
    InfoTile* storageTile_ = nullptr;
    QButtonGroup* locationGroup_ = nullptr;
    std::map<std::string, QPushButton*> locationButtons_;
    WrappedLabel* storageStatus_ = nullptr;
    Button* sendExistingButton_ = nullptr;
    Button* bringBackButton_ = nullptr;
    Button* retryRecordsButton_ = nullptr;
    SettingTile* shareTile_ = nullptr;
    WrappedLabel* hostStatus_ = nullptr;
    WrappedLabel* hostIdentity_ = nullptr;
    QWidget* hostNotes_ = nullptr;
    QVBoxLayout* hostNotesLayout_ = nullptr;
    SettingTile* tailscaleTile_ = nullptr;
    SettingTile* managementTile_ = nullptr;
    SettingTile* keepRecordsTile_ = nullptr;
    SettingTile* manageMcpTile_ = nullptr;
    NoWheelSpinBox* portSpin_ = nullptr;
    QTimer* portTimer_ = nullptr;
    FieldTile* portTile_ = nullptr;
    InfoTile* devicesTile_ = nullptr;
    QWidget* pairingBox_ = nullptr;
    QLabel* pairingCodeLabel_ = nullptr;
    WrappedLabel* pairingExpiryLabel_ = nullptr;
    Button* cancelPairingButton_ = nullptr;
    Button* pairDeviceButton_ = nullptr;
    QWidget* devicesList_ = nullptr;
    QVBoxLayout* devicesLayout_ = nullptr;
    Button* recoverRecordsButton_ = nullptr;
    WrappedLabel* recoveryMessage_ = nullptr;
    QTimer* countdown_ = nullptr;
};

} // namespace fv
