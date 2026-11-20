#include "ui_remote/remote_engine_page.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaMethod>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "base/log.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "ui_remote/record_recovery_dialog.h"
#include "ui_remote/remote_name_dialog.h"
#include "ui_remote/remote_text.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_remote"};

using qt::toQString;
using qt::toUtf8;

QString q(std::string_view text) {
    return toQString(text);
}

// Tiles that show or hide as one tab (_TileSection); their grids still reflow with the page.
QWidget* tileSection(QVBoxLayout*& column) {
    auto* section = new QWidget;
    section->setObjectName(QStringLiteral("remoteSection"));
    column = new QVBoxLayout(section);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);
    return section;
}

// _set_tile_id: not setObjectName, which the theme styles tiles by ("settingsTile").
void setTileId(QWidget* tile, const char* id) {
    ui::setStyleProperty(tile, ui::prop::tileId, QString::fromLatin1(id));
}

// A SettingTile's switch set from saved state without saving it again, and its "checked"
// property kept in step (SettingTile._sync_checked_property).
void mirrorSwitch(SettingTile* tile, bool checked) {
    QAbstractButton* box = tile->checkbox();
    const bool blocked = box->blockSignals(true);
    box->setChecked(checked);
    ui::setStyleProperty(tile, ui::prop::checked, checked);
    box->blockSignals(blocked);
}

// Takes every widget out of a layout; hidden first, since deleteLater waits for the event loop
// and a row taken out of the layout would paint over the new ones until then.
void clearRows(QVBoxLayout* layout, bool hideFirst) {
    while (layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            if (hideFirst) {
                widget->hide();
            }
            widget->deleteLater();
        }
        delete item;
    }
}

// The class name of an exception without its namespaces, as Python's type(exc).__name__.
QString className(const std::exception& error) {
    std::string name = type_display_name(typeid(error));
    if (const auto colon = name.rfind("::"); colon != std::string::npos) {
        name = name.substr(colon + 2);
    }
    return q(name);
}

// str(exc) or type(exc).__name__.
QString errorText(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& caught) {
        const QString text = toQString(caught.what());
        return text.isEmpty() ? className(caught) : text;
    } catch (...) {
        return QStringLiteral("Exception");
    }
}

bool hasDigit(const QString& code) {
    for (const char32_t point : code.toUcs4()) {
        if (py::isdigit(toUtf8(QString::fromUcs4(&point, 1)))) {
            return true;
        }
    }
    return false;
}

RemoteEnginePrompts withDefaults(RemoteEnginePrompts prompts) {
    if (!prompts.askName) {
        prompts.askName = &RemoteNameDialog::ask;
    }
    if (!prompts.question) {
        prompts.question = [](QWidget* parent, const QString& title, const QString& text) {
            return QMessageBox::question(parent, title, text) == QMessageBox::Yes;
        };
    }
    if (!prompts.warning) {
        prompts.warning = [](QWidget* parent, const QString& title, const QString& text) {
            QMessageBox::warning(parent, title, text);
        };
    }
    if (!prompts.information) {
        prompts.information = [](QWidget* parent, const QString& title, const QString& text) {
            QMessageBox::information(parent, title, text);
        };
    }
    if (!prompts.removeDevice) {
        prompts.removeDevice = [](QWidget* parent, const QString& name, const QString& stored) {
            QMessageBox box(parent);
            box.setIcon(QMessageBox::Question);
            box.setWindowTitle(QStringLiteral("Remove paired computer"));
            box.setText(QStringLiteral("%1 keeps %2 here.").arg(name, stored));
            box.setInformativeText(QStringLiteral(
                "Keep them in this computer's History and Past Meetings, or delete them for good? "
                "Deleted records can't be recovered, and that computer can't bring them back."));
            QPushButton* keep =
                box.addButton(QStringLiteral("Keep its records"), QMessageBox::AcceptRole);
            QPushButton* drop =
                box.addButton(QStringLiteral("Delete its records"), QMessageBox::DestructiveRole);
            box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(keep);
            box.exec();
            QAbstractButton* clicked = box.clickedButton();
            if (clicked == keep) {
                return RemoteEnginePrompts::RemoveChoice::KeepRecords;
            }
            if (clicked == drop) {
                return RemoteEnginePrompts::RemoveChoice::DeleteRecords;
            }
            return RemoteEnginePrompts::RemoveChoice::Cancel;
        };
    }
    if (!prompts.recoverRecords) {
        prompts.recoverRecords = &RecordRecoveryDialog::choose;
    }
    return prompts;
}

} // namespace

template <class Work, class Done>
void RemoteEnginePage::run(qt::TokenGate& gate, Work work, Done done, bool latestOnly) {
    using Result = std::invoke_result_t<Work&>;
    const std::uint64_t token = latestOnly ? gate.next() : 0;
    qt::TokenGate latest = gate;
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    auto job = std::make_shared<std::pair<Work, Done>>(std::move(work), std::move(done));
    services_.io->post([ui, latest, token, job] {
        auto result = std::make_shared<Result>();
        std::exception_ptr error;
        try {
            *result = job->first();
        } catch (...) {
            error = std::current_exception();
        }
        ui->post([latest, token, job, result, error] {
            if (token == 0 || latest.isCurrent(token)) {
                job->second(std::move(*result), error);
            }
        });
    });
}

template <class Work, class Done> void RemoteEnginePage::run(Work work, Done done) {
    run(actionGate_, std::move(work), std::move(done), false);
}

template <class Work> void RemoteEnginePage::runDetached(Work work) {
    services_.io->post([work = std::make_shared<Work>(std::move(work))] { (*work)(); });
}

RemoteEnginePage::RemoteEnginePage(SettingsPage& page, RemoteEnginePageServices services,
                                   QWidget* parent)
    : QWidget(parent), services_(std::move(services)), service_(services_.service),
      records_(services_.records),
      clock_(services_.clock != nullptr ? services_.clock : &system_clock()),
      ui_(std::make_shared<qt::QtUiExecutor>(this)) {
    services_.prompts = withDefaults(std::move(services_.prompts));
    if (service_ != nullptr && services_.io == nullptr) {
        throw std::invalid_argument("RemoteEnginePage needs an io executor with a service");
    }
    countdown_ = new QTimer(this);
    countdown_->setInterval(1000);
    connect(countdown_, &QTimer::timeout, this, &RemoteEnginePage::refreshPairingCode);
    build(page);
    if (service_ != nullptr) {
        serviceSubscription_ = service_->changed.connect(
            *ui_, [this](const std::string& kind) { onServiceEvent(kind); });
        if (records_ != nullptr) {
            recordsSubscription_ = records_->changed.connect(
                *ui_, [this](const std::string& kind) { onRecordsEvent(kind); });
        }
    }
    if (services_.speech) {
        speechSubscription_ = services_.speech->readiness_changed.connect(
            *ui_, [this](speech::Slot slot, const speech::Readiness&) {
                if (slot == speech::Slot::Dictation) {
                    renderDictationLink();
                }
            });
    }
    refresh();
}

RemoteEnginePage::~RemoteEnginePage() {
    // A request still waiting on the host's owner would hold an io thread until they answered
    // or the host went away; OpenWhisper leaves its thread waiting.
    if (request_) {
        request_->cancel.cancel();
    }
}

QPushButton* RemoteEnginePage::locationButton(const std::string& location) const {
    const auto found = locationButtons_.find(location);
    return found == locationButtons_.end() ? nullptr : found->second;
}

std::chrono::steady_clock::time_point RemoteEnginePage::now() const {
    return clock_->steady();
}

// ---- construction ----

void RemoteEnginePage::build(SettingsPage& page) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Two audiences share this page: someone using another computer's engine, and someone
    // sharing this one. Tabs keep each to one screen.
    tabs_ =
        new SegmentedBar({{QStringLiteral("Use another computer"), QStringLiteral("Not paired")},
                          {QStringLiteral("Share this computer"), QStringLiteral("Off")}});
    tabs_->setAccessibleName(QStringLiteral("Remote engine role"));
    QVBoxLayout* clientColumn = nullptr;
    QVBoxLayout* hostColumn = nullptr;
    clientContainer_ = tileSection(clientColumn);
    hostContainer_ = tileSection(hostColumn);
    hostContainer_->hide();
    connect(tabs_, &SegmentedBar::activated, this, [this](int) { tabChosen_ = true; });
    connect(tabs_, &SegmentedBar::currentIndexChanged, this, &RemoteEnginePage::showSection);
    layout->addWidget(tabs_);
    layout->addWidget(clientContainer_);
    layout->addWidget(hostContainer_);
    layout->addStretch();

    buildClient(page);
    page.addTileGroup(clientColumn, QString(), {findTile_, clientTile_, storageTile_}, 1,
                      QStringLiteral("Dictate or record meetings here while a faster computer does "
                                     "the transcription. On that computer, open Share this "
                                     "computer and turn on sharing; it then shows up below."));
    buildHost(page);
    page.addTileGroup(hostColumn, QString(),
                      {shareTile_, managementTile_, keepRecordsTile_, manageMcpTile_,
                       tailscaleTile_, portTile_, devicesTile_},
                      1);
    for (Button* button : {pairButton_, useButton_, pairDeviceButton_}) {
        compactPrimaryButton(button);
    }
    for (Button* button :
         {manageButton_, renameHostButton_, forgetButton_, meetingUseButton_, sendExistingButton_,
          bringBackButton_, retryRecordsButton_, searchButton_, sweepButton_, manualButton_,
          waitCancelButton_, cancelPairingButton_, recoverRecordsButton_}) {
        neutralButton(button);
    }
}

void RemoteEnginePage::buildClient(SettingsPage&) {
    // Client, before pairing: the computers nearby that share their engine, found on this
    // network and the tailnet; or an address and code typed in.
    findTile_ = new InfoTile(QStringLiteral("Find a computer"), QString(),
                             ui::designIcon(QStringLiteral("world-blue.svg")));
    setTileId(findTile_, "remoteFindTile");
    findList_ = new QWidget;
    findList_->setObjectName(QStringLiteral("remoteFindList"));
    findLayout_ = new QVBoxLayout(findList_);
    findLayout_->setContentsMargins(0, 0, 0, 0);
    findLayout_->setSpacing(6);
    findTile_->addBody(findList_);

    // While the host's owner decides on a pairing request: the number both screens show, and a
    // way to stop waiting.
    waitBox_ = new QWidget;
    waitBox_->setObjectName(QStringLiteral("remoteWaitBox"));
    auto* waitLayout = new QHBoxLayout(waitBox_);
    waitLayout->setContentsMargins(0, 0, 0, 0);
    waitLayout->setSpacing(12);
    waitCodeLabel_ = new QLabel(QString());
    waitCodeLabel_->setObjectName(QStringLiteral("remoteWaitCode"));
    waitCodeLabel_->setAccessibleName(QStringLiteral("Pairing number"));
    waitLabel_ = new WrappedLabel(QString());
    waitLabel_->setObjectName(QStringLiteral("remoteWaitLabel"));
    waitCancelButton_ = new Button(QStringLiteral("Cancel"));
    waitCancelButton_->setObjectName(QStringLiteral("remoteWaitCancelButton"));
    connect(waitCancelButton_, &QPushButton::clicked, this, &RemoteEnginePage::cancelRequest);
    waitLayout->addWidget(waitCodeLabel_);
    waitLayout->addWidget(waitLabel_, 1);
    waitLayout->addWidget(waitCancelButton_);
    waitBox_->hide();
    findTile_->addBody(waitBox_);

    auto* findActions = new QHBoxLayout;
    findActions->setContentsMargins(0, 0, 0, 0);
    searchButton_ = new Button(QStringLiteral("Search again"));
    searchButton_->setObjectName(QStringLiteral("remoteSearchButton"));
    connect(searchButton_, &QPushButton::clicked, this, [this] { scanNearby(true); });
    sweepButton_ = new Button(QStringLiteral("Check every address"));
    sweepButton_->setObjectName(QStringLiteral("remoteSweepButton"));
    sweepButton_->setToolTip(QStringLiteral(
        "Try each address on this network, for routers that hide computers from a search"));
    connect(sweepButton_, &QPushButton::clicked, this, [this] { scanNearby(true, true); });
    manualButton_ = new Button(QStringLiteral("Enter an address"));
    manualButton_->setObjectName(QStringLiteral("remoteManualButton"));
    connect(manualButton_, &QPushButton::clicked, this, &RemoteEnginePage::showManual);
    findActions->addWidget(searchButton_);
    findActions->addWidget(sweepButton_);
    findActions->addWidget(manualButton_);
    findActions->addStretch(1);
    findTile_->addBodyLayout(findActions);

    // Client: pair with a host, then select the engine.
    clientTile_ = new InfoTile(QStringLiteral("Paired host"), QString(),
                               ui::designIcon(QStringLiteral("server-blue.svg")));
    setTileId(clientTile_, "remoteClientTile");
    pairRow_ = new QWidget;
    pairRow_->setObjectName(QStringLiteral("remotePairRow"));
    auto* pairLayout = new QHBoxLayout(pairRow_);
    pairLayout->setContentsMargins(0, 0, 0, 0);
    pairLayout->setSpacing(8);
    addressEdit_ = new QLineEdit;
    addressEdit_->setObjectName(QStringLiteral("remoteAddressEdit"));
    addressEdit_->setPlaceholderText(
        QStringLiteral("Host address, e.g. 192.168.1.20 or devbox.local"));
    addressEdit_->setMinimumHeight(36);
    codeEdit_ = new QLineEdit;
    codeEdit_->setObjectName(QStringLiteral("remoteCodeEdit"));
    codeEdit_->setPlaceholderText(QStringLiteral("Pairing code"));
    codeEdit_->setMaxLength(12);
    codeEdit_->setMinimumHeight(36);
    codeEdit_->setMaximumWidth(150);
    connect(codeEdit_, &QLineEdit::returnPressed, this, &RemoteEnginePage::pair);
    pairButton_ = new PrimaryButton(QStringLiteral("Pair"));
    pairButton_->setObjectName(QStringLiteral("remotePairButton"));
    connect(pairButton_, &QPushButton::clicked, this, &RemoteEnginePage::pair);
    pairLayout->addWidget(addressEdit_, 1);
    pairLayout->addWidget(codeEdit_);
    pairLayout->addWidget(pairButton_);
    pairRow_->hide();
    findTile_->addBody(pairRow_);
    findMessage_ = new WrappedLabel(QString());
    findMessage_->setObjectName(QStringLiteral("remoteFindMessage"));
    findTile_->addBody(findMessage_);

    credentialRecovery_ = new QWidget;
    credentialRecovery_->setObjectName(QStringLiteral("remoteCredentialRecovery"));
    auto* recoveryLayout = new QVBoxLayout(credentialRecovery_);
    recoveryLayout->setContentsMargins(0, 0, 0, 0);
    recoveryLayout->setSpacing(8);
    credentialHelp_ = new WrappedLabel(QString());
    credentialHelp_->setObjectName(QStringLiteral("remoteCredentialHelp"));
    credentialHelp_->setTextFormat(Qt::PlainText);
    recoveryLayout->addWidget(credentialHelp_);
    // "Retry Keychain access" and "Open Keychain Access" are macOS's; the button stays hidden
    // here, as OpenWhisper builds it everywhere and shows it only there.
    retryKeyButton_ = neutralButton(new Button(QStringLiteral("Retry saving key")));
    retryKeyButton_->setObjectName(QStringLiteral("remoteRetryKeyButton"));
    connect(retryKeyButton_, &QPushButton::clicked, this, &RemoteEnginePage::retryPairingSave);
    openKeychainButton_ = neutralButton(new Button(QStringLiteral("Open Keychain Access")));
    openKeychainButton_->setObjectName(QStringLiteral("remoteOpenKeychainButton"));
    openKeychainButton_->setVisible(false);
    auto* credentialActions = new ButtonRow({retryKeyButton_, openKeychainButton_});
    credentialActions->setObjectName(QStringLiteral("remoteCredentialActions"));
    recoveryLayout->addWidget(credentialActions);
    credentialRecovery_->hide();
    findTile_->addBody(credentialRecovery_);

    pairedRow_ = new QWidget;
    pairedRow_->setObjectName(QStringLiteral("remotePairedRow"));
    auto* pairedLayout = new QVBoxLayout(pairedRow_);
    pairedLayout->setContentsMargins(0, 0, 0, 0);
    pairedLayout->setSpacing(8);
    useButton_ = new PrimaryButton(QStringLiteral("Use for dictation"));
    useButton_->setObjectName(QStringLiteral("remoteUseButton"));
    connect(useButton_, &QPushButton::clicked, this, &RemoteEnginePage::useEngine);
    manageButton_ = new Button(QStringLiteral("Manage host models"));
    manageButton_->setObjectName(QStringLiteral("remoteManageModelsButton"));
    connect(manageButton_, &QPushButton::clicked, this, &RemoteEnginePage::manageModels);
    renameHostButton_ = new Button(QStringLiteral("Rename host"));
    renameHostButton_->setObjectName(QStringLiteral("remoteRenameHostButton"));
    renameHostButton_->setToolTip(
        QStringLiteral("Change the name this computer shows for the host"));
    connect(renameHostButton_, &QPushButton::clicked, this, &RemoteEnginePage::renameHost);
    forgetButton_ = new Button(QStringLiteral("Forget host"));
    forgetButton_->setObjectName(QStringLiteral("remoteForgetButton"));
    connect(forgetButton_, &QPushButton::clicked, this, &RemoteEnginePage::forget);
    auto* useRow = new QHBoxLayout;
    useRow->addWidget(useButton_);
    if (services_.speech && services_.usesRemoteDictation && services_.retryRemoteDictation) {
        retryDictation_ = new Button(tr("Retry dictation connection"));
        retryDictation_->setObjectName(QStringLiteral("remoteRetryDictation"));
        retryDictation_->hide();
        neutralButton(retryDictation_);
        connect(retryDictation_, &QPushButton::clicked, this, [this] {
            if (services_.usesRemoteDictation()) {
                services_.retryRemoteDictation();
                renderDictationLink();
            }
        });
        useRow->addWidget(retryDictation_);
    }
    meetingUseButton_ = new Button(QStringLiteral("Use for meetings"));
    meetingUseButton_->setObjectName(QStringLiteral("remoteUseForMeetingsButton"));
    connect(meetingUseButton_, &QPushButton::clicked, this, &RemoteEnginePage::useForMeetings);
    useRow->addWidget(meetingUseButton_);
    useRow->addStretch(1);
    pairedLayout->addLayout(useRow);
    if (retryDictation_) {
        dictationLink_ = new QLabel(pairedRow_);
        dictationLink_->setWordWrap(true);
        dictationLink_->setTextFormat(Qt::PlainText);
        dictationLink_->hide();
        pairedLayout->addWidget(dictationLink_);
    }
    auto* manageRow = new QHBoxLayout;
    manageRow->addWidget(manageButton_);

    manageRow->addWidget(renameHostButton_);
    manageRow->addWidget(forgetButton_);
    manageRow->addStretch(1);
    pairedLayout->addLayout(manageRow);
    if (services_.canBrowseHeldMeetings) {
        auto* meetings = new QHBoxLayout;
        auto* browse = neutralButton(new Button(tr("Browse host-kept meetings")));
        browse->setObjectName(QStringLiteral("remoteBrowseHeldMeetings"));
        connect(browse, &QPushButton::clicked, this, [this] {
            if (snapshot_.pairing) {
                Q_EMIT heldMeetingsRequested(q(snapshot_.pairing->host_name),
                                             q(snapshot_.pairing->fingerprint));
            }
        });
        auto* share = neutralButton(new Button(tr("Share a finished meeting")));
        share->setObjectName(QStringLiteral("remoteShareFinishedMeeting"));
        connect(share, &QPushButton::clicked, this, [this] {
            if (snapshot_.pairing) {
                Q_EMIT shareMeetingRequested(q(snapshot_.pairing->host_name),
                                             q(snapshot_.pairing->fingerprint));
            }
        });
        meetings->addWidget(browse);
        meetings->addWidget(share);
        meetings->addStretch();
        pairedLayout->addLayout(meetings);
    }
    if (services_.canPublishHistory) {
        auto* publish = neutralButton(new Button(tr("Share history for this session…")));
        publish->setObjectName(QStringLiteral("remotePublishHistory"));
        publishHistory_ = publish;
        connect(publish, &QPushButton::clicked, this, [this] {
            if (snapshot_.pairing) {
                Q_EMIT publishHistoryRequested(q(snapshot_.pairing->host_name),
                                               q(snapshot_.pairing->fingerprint));
            }
        });
        pairedLayout->addWidget(publish, 0, Qt::AlignLeft);
    }
    clientTile_->addBody(pairedRow_);

    clientMessage_ = new WrappedLabel(QString());
    clientMessage_->setObjectName(QStringLiteral("remoteClientMessage"));
    clientTile_->addBody(clientMessage_);
    // Paired on this network only: how to keep using it away from home.
    tailscaleHint_ = new WrappedLabel(QString());
    tailscaleHint_->setObjectName(QStringLiteral("remoteTailscaleHint"));
    tailscaleHint_->setTextFormat(Qt::RichText);
    // Links open through openUrl, so nothing leaves the app unless the page was given an opener.
    tailscaleHint_->setOpenExternalLinks(false);
    connect(tailscaleHint_, &QLabel::linkActivated, this, &RemoteEnginePage::openUrl);
    tailscaleHint_->hide();
    clientTile_->addBody(tailscaleHint_);

    // Client: where this computer's history, recordings and meetings go.
    storageTile_ = new InfoTile(QStringLiteral("Where records are kept"), QString(),
                                ui::designIcon(QStringLiteral("box-blue.svg")));
    setTileId(storageTile_, "remoteStorageTile");
    auto* switchFrame = new QFrame;
    switchFrame->setObjectName(QStringLiteral("recordsLocationSwitch"));
    auto* switchLayout = new QHBoxLayout(switchFrame);
    switchLayout->setContentsMargins(2, 2, 2, 2);
    switchLayout->setSpacing(2);
    locationGroup_ = new QButtonGroup(this);
    const std::array<const char*, 3> labels = {"This computer", "Host", "Both"};
    for (std::size_t i = 0; i < remote::RECORD_LOCATIONS.size(); ++i) {
        const std::string location(remote::RECORD_LOCATIONS[i]);
        auto* button = new QPushButton(QString::fromLatin1(labels[i]));
        button->setObjectName(QStringLiteral("recordsLocationBtn"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(30);
        connect(button, &QPushButton::clicked, this,
                [this, location] { chooseLocation(location); });
        locationGroup_->addButton(button);
        switchLayout->addWidget(button);
        locationButtons_[location] = button;
    }
    auto* switchRow = new QHBoxLayout;
    switchRow->setContentsMargins(0, 0, 0, 0);
    switchRow->addWidget(switchFrame);
    switchRow->addStretch(1);
    storageTile_->addBodyLayout(switchRow);
    storageStatus_ = new WrappedLabel(QString());
    storageStatus_->setObjectName(QStringLiteral("remoteStorageStatus"));
    storageTile_->addBody(storageStatus_);
    auto* storageActions = new QHBoxLayout;
    storageActions->setContentsMargins(0, 0, 0, 0);
    sendExistingButton_ = new Button(QStringLiteral("Move existing records"));
    sendExistingButton_->setObjectName(QStringLiteral("remoteSendExistingButton"));
    connect(sendExistingButton_, &QPushButton::clicked, this, &RemoteEnginePage::sendExisting);
    bringBackButton_ = new Button(QStringLiteral("Bring records back"));
    bringBackButton_->setObjectName(QStringLiteral("remoteBringBackButton"));
    connect(bringBackButton_, &QPushButton::clicked, this, &RemoteEnginePage::bringBack);
    retryRecordsButton_ = new Button(QStringLiteral("Try again"));
    retryRecordsButton_->setObjectName(QStringLiteral("remoteRetryRecordsButton"));
    connect(retryRecordsButton_, &QPushButton::clicked, this, [this] {
        if (records_ != nullptr) {
            runDetached([records = records_] {
                try {
                    records->wake(true);
                } catch (const std::exception& error) {
                    kLog.warning("Couldn't wake the record sync: {}", error.what());
                }
            });
        }
    });
    storageActions->addWidget(sendExistingButton_);
    storageActions->addWidget(bringBackButton_);
    storageActions->addWidget(retryRecordsButton_);
    storageActions->addStretch(1);
    storageTile_->addBodyLayout(storageActions);
}

void RemoteEnginePage::buildHost(SettingsPage&) {
    // Host: share the engine selected on this computer.
    shareTile_ = new SettingTile(
        QStringLiteral("Share this computer's engine"),
        QStringLiteral("Paired computers can dictate or transcribe meetings with the engine "
                       "selected here, and switch it to any model downloaded here. The connection "
                       "is encrypted and only computers you pair can use it. Windows may ask once "
                       "to allow private networks."),
        ui::designIcon(QStringLiteral("server-blue.svg")));
    setTileId(shareTile_, "remoteShareTile");
    connect(shareTile_->checkbox(), &QAbstractButton::toggled, this,
            &RemoteEnginePage::onShareToggled);
    hostStatus_ = new WrappedLabel(QString());
    hostStatus_->setObjectName(QStringLiteral("remoteHostStatus"));
    shareTile_->addBody(hostStatus_);
    hostIdentity_ = new WrappedLabel(QString());
    hostIdentity_->setObjectName(QStringLiteral("remoteHostIdentity"));
    hostIdentity_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shareTile_->addBody(hostIdentity_);
    // What may keep other computers out (the service's reachability notes).
    hostNotes_ = new QWidget;
    hostNotes_->setObjectName(QStringLiteral("remoteHostNotes"));
    hostNotesLayout_ = new QVBoxLayout(hostNotes_);
    hostNotesLayout_->setContentsMargins(0, 0, 0, 0);
    hostNotesLayout_->setSpacing(6);
    hostNotes_->hide();
    shareTile_->addBody(hostNotes_);

    tailscaleTile_ = new SettingTile(QStringLiteral("Pair my Tailscale computers without a code"),
                                     QString(), ui::designIcon(QStringLiteral("world-blue.svg")));
    setTileId(tailscaleTile_, "remoteTailscaleTile");
    connect(tailscaleTile_->checkbox(), &QAbstractButton::toggled, this, [this](bool checked) {
        setPermission(
            [checked](remote::RemoteEngineControl& service) {
                service.set_tailscale_trust(checked);
            },
            [checked](remote::HostState& state) { state.tailscale_trust = checked; });
    });

    managementTile_ = new SettingTile(
        QStringLiteral("Allow paired computers to manage models"),
        QStringLiteral("Paired computers can browse the model catalog and download models and "
                       "verified speech runtimes here, using this computer's network and storage. "
                       "Its Hugging Face download policy still applies. Turning this off blocks "
                       "new requests; downloads already started continue. Choosing an "
                       "already-downloaded model never needs it."),
        ui::designIcon(QStringLiteral("server-blue.svg")));
    setTileId(managementTile_, "remoteModelManagementTile");
    connect(managementTile_->checkbox(), &QAbstractButton::toggled, this, [this](bool checked) {
        setPermission(
            [checked](remote::RemoteEngineControl& service) {
                service.set_model_management(checked);
            },
            [checked](remote::HostState& state) { state.model_management = checked; });
    });

    keepRecordsTile_ = new SettingTile(
        QStringLiteral("Keep records for paired computers"),
        QStringLiteral("Paired computers can keep their dictation history, recordings and meetings "
                       "here. They show in History and Past Meetings, marked with the computer "
                       "they came from, and each computer can see and delete only its own. "
                       "Turning this off stops new ones; what's kept stays until that computer "
                       "brings it back or you remove it below."),
        ui::designIcon(QStringLiteral("box-blue.svg")));
    setTileId(keepRecordsTile_, "remoteKeepRecordsTile");
    connect(keepRecordsTile_->checkbox(), &QAbstractButton::toggled, this, [this](bool checked) {
        setPermission(
            [checked](remote::RemoteEngineControl& service) { service.set_keep_records(checked); },
            [checked](remote::HostState& state) { state.keep_records = checked; });
    });

    const auto intro =
        services_.canGrantMcpSession
            ? tr("Allow one named paired computer to start or stop a temporary, read-only MCP "
                 "listener on this computer. The listener binds only to 127.0.0.1:8768 for "
                 "up to 15 minutes. The peer never receives the token.")
            : tr("Paired computers can turn this computer's MCP server on or off, change "
                 "what assistants may do, and copy its access token. That token can read "
                 "every saved dictation and meeting here, including other computers' "
                 "records. Turning this off blocks new requests at once.");
    manageMcpTile_ = new SettingTile(services_.canGrantMcpSession
                                         ? tr("Grant session MCP control to one paired computer")
                                         : QStringLiteral("Allow paired computers to manage MCP"),
                                     intro, ui::designIcon(QStringLiteral("key-blue.svg")));
    setTileId(manageMcpTile_, "remoteManageMcpTile");
    connect(manageMcpTile_->checkbox(), &QAbstractButton::toggled, this, [this](bool checked) {
        if (checked && services_.canGrantMcpSession) {
            mirrorSwitch(manageMcpTile_, false);
            const auto devices = snapshot_.host.devices;
            if (!snapshot_.host.running || devices.empty()) {
                return;
            }
            QInputDialog picker(this);
            picker.setWindowTitle(tr("Choose the paired computer"));
            picker.setLabelText(tr("Which paired computer may manage this MCP session?"));
            QStringList labels;
            for (const auto& device : devices) {
                labels.push_back(tr("%1 (%2)").arg(q(device.name), q(device.id)));
            }
            picker.setComboBoxItems(labels);
            picker.setComboBoxEditable(false);
            if (picker.exec() != QDialog::Accepted) {
                return;
            }
            const auto selection = labels.indexOf(picker.textValue());
            if (selection < 0) {
                return;
            }
            const auto device = devices.at(static_cast<std::size_t>(selection));
            QMessageBox prompt(
                QMessageBox::Question, tr("Grant MCP session control?"),
                tr("Let %1 start and stop a read-only MCP listener at 127.0.0.1:8768 for "
                   "up to 15 minutes? Apps here with the temporary token can read saved "
                   "dictations, transcripts and meeting insights while it runs. This excludes "
                   "copies kept for paired computers.")
                    .arg(q(device.name)),
                QMessageBox::Yes | QMessageBox::Cancel, this);
            prompt.setTextFormat(Qt::PlainText);
            prompt.setDefaultButton(QMessageBox::Cancel);
            if (prompt.exec() == QMessageBox::Yes) {
                setPermission(
                    [id = device.id](remote::RemoteEngineControl& service) {
                        service.grant_mcp_session(id);
                    },
                    [](remote::HostState& state) { state.manage_mcp = true; });
            }
            return;
        }
        setPermission(
            [checked](remote::RemoteEngineControl& service) { service.set_manage_mcp(checked); },
            [checked](remote::HostState& state) { state.manage_mcp = checked; });
    });

    if (services_.canGrantMcpSession) {
        auto* connection = neutralButton(new Button(tr("Show local MCP session connection")));
        manageMcpTile_->addBody(connection);
        connect(connection, &QPushButton::clicked, this, [this] {
            run([service = service_] { return service->local_mcp_session_token(); },
                [this](const std::string& token, std::exception_ptr) {
                    QMessageBox details(
                        QMessageBox::Information, tr("Local MCP session"),
                        token.empty()
                            ? tr("No peer-managed local MCP listener is running.")
                            : tr("URL: http://127.0.0.1:8768/mcp\nSession token: %1").arg(q(token)),
                        QMessageBox::Ok, this);
                    details.setTextFormat(Qt::PlainText);
                    details.setTextInteractionFlags(Qt::TextSelectableByMouse);
                    details.exec();
                });
        });
    }

    portSpin_ = new NoWheelSpinBox;
    portSpin_->setObjectName(QStringLiteral("remotePortSpin"));
    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(remote::DEFAULT_PORT);
    portSpin_->setMinimumHeight(40);
    portSpin_->setMinimumWidth(120);
    portSpin_->setKeyboardTracking(false);
    connect(portSpin_, &QSpinBox::valueChanged, this, &RemoteEnginePage::onPortChanged);
    portTimer_ = new QTimer(portSpin_);
    portTimer_->setSingleShot(true);
    portTimer_->setInterval(remote_text::PORT_SETTLE_MS);
    connect(portTimer_, &QTimer::timeout, this, &RemoteEnginePage::applyPort);
    portTile_ = new FieldTile(QStringLiteral("Port"),
                              QStringLiteral("Clients connect to this port. %1 unless something "
                                             "else already uses it.")
                                  .arg(remote::DEFAULT_PORT),
                              portSpin_, ui::designIcon(QStringLiteral("bolt-green.svg")), true);

    devicesTile_ = new InfoTile(
        QStringLiteral("Paired computers"),
        QStringLiteral("Computers that can use this engine. One that finds this computer can ask "
                       "to pair, and you're asked here to allow it. Removing one cuts it off at "
                       "once."),
        ui::designIcon(QStringLiteral("key-blue.svg")));
    setTileId(devicesTile_, "remoteDevicesTile");
    pairingBox_ = new QWidget;
    pairingBox_->setObjectName(QStringLiteral("remotePairingBox"));
    auto* pairingLayout = new QHBoxLayout(pairingBox_);
    pairingLayout->setContentsMargins(0, 0, 0, 0);
    pairingLayout->setSpacing(12);
    pairingCodeLabel_ = new QLabel(QString());
    pairingCodeLabel_->setObjectName(QStringLiteral("remotePairingCode"));
    for (QLabel* label : {pairingCodeLabel_, waitCodeLabel_}) {
        // As OpenWhisper sizes them: from the label's font as constructed.
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() * 1.8);
        font.setBold(true);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        label->setFont(font);
    }
    pairingExpiryLabel_ = new WrappedLabel(QString());
    pairingExpiryLabel_->setObjectName(QStringLiteral("remotePairingExpiry"));
    cancelPairingButton_ = new Button(QStringLiteral("Cancel"));
    cancelPairingButton_->setObjectName(QStringLiteral("remoteCancelPairingButton"));
    connect(cancelPairingButton_, &QPushButton::clicked, this, &RemoteEnginePage::cancelPairing);
    pairingLayout->addWidget(pairingCodeLabel_);
    pairingLayout->addWidget(pairingExpiryLabel_, 1);
    pairingLayout->addWidget(cancelPairingButton_);
    devicesTile_->addBody(pairingBox_);
    pairDeviceButton_ = new PrimaryButton(QStringLiteral("Show a pairing code"));
    pairDeviceButton_->setToolTip(
        QStringLiteral("For a computer that enters this one's address, or runs an older %1")
            .arg(q(identity().product)));
    pairDeviceButton_->setObjectName(QStringLiteral("remotePairDeviceButton"));
    connect(pairDeviceButton_, &QPushButton::clicked, this, &RemoteEnginePage::openPairing);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addWidget(pairDeviceButton_);
    buttonRow->addStretch(1);
    devicesTile_->addBodyLayout(buttonRow);
    devicesList_ = new QWidget;
    devicesList_->setObjectName(QStringLiteral("remoteDevicesList"));
    devicesLayout_ = new QVBoxLayout(devicesList_);
    devicesLayout_->setContentsMargins(0, 0, 0, 0);
    devicesLayout_->setSpacing(6);
    devicesTile_->addBody(devicesList_);
    recoverRecordsButton_ = new Button(QStringLiteral("Recover stored records…"));
    recoverRecordsButton_->setObjectName(QStringLiteral("remoteRecoverRecordsButton"));
    recoverRecordsButton_->setToolTip(
        QStringLiteral("Reconnect records from an old pairing to a paired computer"));
    connect(recoverRecordsButton_, &QPushButton::clicked, this, &RemoteEnginePage::recoverRecords);
    auto* recoverRow = new QHBoxLayout;
    recoverRow->setContentsMargins(0, 0, 0, 0);
    recoverRow->addWidget(recoverRecordsButton_);
    recoverRow->addStretch(1);
    devicesTile_->addBodyLayout(recoverRow);
    recoveryMessage_ = new WrappedLabel(QString());
    recoveryMessage_->setObjectName(QStringLiteral("infoLabel"));
    recoveryMessage_->setTextFormat(Qt::PlainText);
    recoveryMessage_->hide();
    devicesTile_->addBody(recoveryMessage_);
}

// ---- reading the state ----

void RemoteEnginePage::refresh() {
    render();
    requestSnapshot();
}

void RemoteEnginePage::requestSnapshot() {
    if (service_ == nullptr) {
        return;
    }
    ++pendingReads_;
    const bool askTailscale = !scan_.has_value();
    run(
        snapshotGate_,
        [service = service_, records = records_, askTailscale] {
            Snapshot read;
            read.pairing = service->client_pairing();
            read.pendingName = service->pending_pairing_name();
            if (askTailscale) {
                read.tailscale = service->tailscale_status();
            }
            read.host = service->host_state();
            if (records != nullptr) {
                try {
                    read.records = records->status();
                } catch (const std::exception& error) {
                    kLog.warning("Record sync status failed: {}", error.what());
                }
            }
            for (const remote::PairedDevice& device : read.host.devices) {
                if (device.id.empty()) {
                    continue;
                }
                try {
                    read.deviceRecords[device.id] = service->records_summary(device.id);
                } catch (const std::exception& error) {
                    kLog.debug("Could not count a paired computer's records: {}", error.what());
                }
            }
            return std::make_optional(std::move(read));
        },
        [this](std::optional<Snapshot> read, std::exception_ptr error) {
            pendingReads_ = 0;
            if (error) {
                kLog.warning("Reading the remote engine's state failed: {}",
                             toUtf8(errorText(error)));
                return;
            }
            snapshot_ = std::move(*read);
            haveSnapshot_ = true;
            render();
        });
}

void RemoteEnginePage::requestRecords() {
    if (records_ == nullptr) {
        renderRecords();
        return;
    }
    run(
        recordsGate_,
        [records = records_]() -> std::optional<remote::SyncStatus> {
            try {
                return records->status();
            } catch (const std::exception& error) {
                kLog.warning("Record sync status failed: {}", error.what());
                return std::nullopt;
            }
        },
        [this](std::optional<remote::SyncStatus> status, std::exception_ptr) {
            snapshot_.records = std::move(status);
            renderRecords();
        });
}

// ---- drawing ----

void RemoteEnginePage::render() {
    if (service_ != nullptr && !haveSnapshot_) {
        // Nothing is known yet: the page stays as built until the first read arrives.
        return;
    }
    for (QWidget* widget : std::initializer_list<QWidget*>{
             clientTile_, findTile_, shareTile_, managementTile_, keepRecordsTile_, manageMcpTile_,
             tailscaleTile_, portTile_, devicesTile_}) {
        widget->setEnabled(service_ != nullptr);
    }
    if (service_ == nullptr) {
        renderUnbound();
        return;
    }
    renderClient();
    renderRecords();
    renderFind();
    renderHost();
}

void RemoteEnginePage::renderUnbound() {
    clientTile_->show();
    clientTile_->setDescription(
        QStringLiteral("The remote engine isn't available in this window."));
    pairedRow_->hide();
    pairingBox_->hide();
    findTile_->hide();
    tailscaleTile_->hide();
    storageTile_->hide();
}

void RemoteEnginePage::renderClient() {
    const std::optional<remote::ClientPairing>& pairing = snapshot_.pairing;
    clientTile_->setVisible(pairing.has_value());
    findTile_->setVisible(!pairing.has_value());
    if (!pairing) {
        pairedRow_->hide();
        tailscaleHint_->hide();
    } else {
        const QString identityText = q(remote::short_fingerprint(pairing->fingerprint));
        QString where;
        if (pairing->over_tailscale()) {
            where = QStringLiteral("over Tailscale at %1").arg(q(pairing->address()));
        } else {
            where = QStringLiteral("at %1").arg(q(pairing->address()));
            const std::string fallback = pairing->tailscale_fallback();
            if (!fallback.empty()) {
                where += QStringLiteral(", or over Tailscale at %1 when away").arg(q(fallback));
            }
        }
        const QString name =
            pairing->renamed()
                ? QStringLiteral("%1 (%2)").arg(q(pairing->host_name), q(pairing->paired_name))
                : q(pairing->host_name);
        clientTile_->setDescription(QStringLiteral("Paired with %1 %2. Its identity is %3.")
                                        .arg(name, where, identityText));
        pairedRow_->show();
        // Paired on this network only. Away from home is Tailscale's job; say so once it's
        // clear the home network works.
        const bool homeOnly = pairing->via != "tailscale" && !pairing->over_tailscale() &&
                              pairing->tailscale_fallback().empty();
        tailscaleHint_->setVisible(homeOnly);
        if (homeOnly) {
            tailscaleHint_->setText(
                QStringLiteral("Works while both computers are on the same network. To use %1 "
                               "away from home too, %2 on both computers and sign in with the "
                               "same account.")
                    .arg(q(pairing->host_name), tailscaleLink(QStringLiteral("get Tailscale"))));
        }
    }
    renderDictationLink();
    pairButton_->setEnabled(!pairingBusy_);
    const QString pendingName = q(snapshot_.pendingName);
    credentialRecovery_->setVisible(!pairing && !pendingName.isEmpty());
    retryKeyButton_->setEnabled(!pairingBusy_);
    if (!pendingName.isEmpty()) {
        credentialHelp_->setText(
            QStringLiteral("Finish pairing with %1. Unlock or enable %2, then click Retry "
                           "saving key.")
                .arg(pendingName, q(platform_backend_name())));
    }
}

void RemoteEnginePage::renderDictationLink() {
    if (!retryDictation_) {
        return;
    }
    const bool selected = snapshot_.pairing && services_.speech && services_.usesRemoteDictation &&
                          services_.usesRemoteDictation();
    useButton_->setVisible(!selected);
    if (!selected) {
        retryDictation_->hide();
        dictationLink_->hide();
        return;
    }
    const auto ready = services_.speech->readiness(speech::Slot::Dictation);
    const auto host = q(snapshot_.pairing->host_name);
    const bool retry =
        ready.state == speech::ReadyState::Offline || ready.state == speech::ReadyState::Failed;
    retryDictation_->setVisible(retry && static_cast<bool>(services_.retryRemoteDictation));
    if (ready.state == speech::ReadyState::Ready) {
        dictationLink_->setText(tr("Dictation is connected to %1.").arg(host));
    } else if (retry) {
        dictationLink_->setText(ready.message.empty()
                                    ? tr("Can't reach %1 for dictation.").arg(host)
                                    : q(ready.message));
    } else {
        dictationLink_->setText(tr("Connecting to %1 for dictation…").arg(host));
    }
    dictationLink_->show();
}

QString RemoteEnginePage::tailscaleLink(const QString& text) const {
    return QStringLiteral("<a href=\"%1\" style=\"color: %2; text-decoration: underline;\">%3</a>")
        .arg(q(remote_text::TAILSCALE_DOWNLOAD_URL), q(ui::currentPalette().css("accent")), text);
}

void RemoteEnginePage::renderRecords() {
    if (service_ == nullptr) {
        return;
    }
    if (!snapshot_.records) {
        storageTile_->hide();
        return;
    }
    const remote::SyncStatus& status = *snapshot_.records;
    storageTile_->setVisible(status.paired);
    if (!status.paired) {
        return;
    }
    const QString host =
        status.host_name.empty() ? QStringLiteral("the host") : q(status.host_name);
    QPushButton* hostButton = locationButtons_.at("host");
    hostButton->setText(hostButton->fontMetrics().elidedText(host, Qt::ElideMiddle, 160));
    hostButton->setToolTip(QStringLiteral("Keep records on %1").arg(host));
    for (const auto& [location, button] : locationButtons_) {
        button->setChecked(location == status.location);
        button->setEnabled(recordsBusy_.isEmpty());
    }
    try {
        if (records_ && records_->manual_transfers()) {
            storageTile_->setDescription(QStringLiteral(
                "Copy or move existing dictations to the paired host when you choose. "
                "New dictations stay here until you send them."));
        } else {
            storageTile_->setDescription(
                q(remote_text::records_location_description(status.location, toUtf8(host))));
        }
    } catch (const std::invalid_argument& error) {
        kLog.warning("Record sync reports an unknown location: {}", error.what());
    }
    const QString stored = q(remote_text::stored_phrase(status.stored));
    QStringList lines;
    recordsStatusLines(status, host, stored, lines);
    storageStatus_->setText(lines.join(QLatin1Char(' ')));
    storageStatus_->setVisible(!lines.isEmpty());
    showRecordActions(status, host, stored);
}

void RemoteEnginePage::recordsStatusLines(const remote::SyncStatus& status, const QString& host,
                                          const QString& stored, QStringList& lines) const {
    if (!recordsBusy_.isEmpty()) {
        lines.append(recordsBusy_);
    } else if (!status.active.empty()) {
        lines.append(q(status.active));
    }
    const QString hostProblem = q(remote_text::records_host_problem(status, toUtf8(host)));
    if (!hostProblem.isEmpty()) {
        lines.append(hostProblem);
    } else if (!status.waiting.empty() && status.active.empty()) {
        lines.append(q(status.waiting));
    }
    if (status.pending != 0 && status.active.empty() && recordsBusy_.isEmpty()) {
        lines.append(QStringLiteral("%1 waiting to be sent.").arg(status.pending));
    }
    if (!status.error.empty() && status.pending != 0 && status.active.empty()) {
        lines.append(q(status.error));
    }
    if (!stored.isEmpty()) {
        lines.append(QStringLiteral("On %1: %2.").arg(host, stored));
    }
    if (!recordsNote_.isEmpty()) {
        lines.append(recordsNote_);
    }
}

void RemoteEnginePage::showRecordActions(const remote::SyncStatus& status, const QString& host,
                                         const QString& stored) {
    const bool blocked = !recordsBusy_.isEmpty();
    sendExistingButton_->setVisible(status.location != "local");
    sendExistingButton_->setText(status.location == "host"
                                     ? QStringLiteral("Move existing records")
                                     : QStringLiteral("Copy existing records"));
    sendExistingButton_->setToolTip(
        QStringLiteral("Send the records already on this computer to %1 too").arg(host));
    sendExistingButton_->setEnabled(!blocked && status.host_keeps != false);
    bringBackButton_->setVisible(!stored.isEmpty());
    bringBackButton_->setToolTip(
        QStringLiteral("Move everything %1 keeps for this computer back here").arg(host));
    bringBackButton_->setEnabled(!blocked);
    retryRecordsButton_->setVisible(status.pending != 0 && !status.error.empty() &&
                                    status.active.empty());
}

void RemoteEnginePage::clearFindRows() {
    clearRows(findLayout_, true);
}

void RemoteEnginePage::findNote(const QString& text, bool link) {
    auto* note = new WrappedLabel(text);
    note->setObjectName(QStringLiteral("remoteFindNote"));
    if (link) {
        note->setTextFormat(Qt::RichText);
        note->setOpenExternalLinks(false);
        connect(note, &QLabel::linkActivated, this, &RemoteEnginePage::openUrl);
    }
    findLayout_->addWidget(note);
}

void RemoteEnginePage::renderFind() {
    if (snapshot_.pairing) {
        return;
    }
    clearFindRows();
    const bool waiting = request_.has_value();
    waitBox_->setVisible(waiting && !request_->sas.isEmpty());
    pairRow_->setVisible(manual_ && !waiting);
    manualButton_->setVisible(!manual_ && !waiting);
    searchButton_->setEnabled(!scanBusy_ && !pairingBusy_);
    sweepButton_->setEnabled(!scanBusy_ && !pairingBusy_);
    const std::optional<net::TailscaleStatus>& status = scan_ ? scan_->status : snapshot_.tailscale;
    const bool tailnet = status && status->running();
    const QString where = tailnet ? QStringLiteral("on this network and your tailnet")
                                  : QStringLiteral("on this network");
    if (scanBusy_) {
        findTile_->setDescription(sweeping_
                                      ? QStringLiteral("Checking every address on this network...")
                                      : QStringLiteral("Looking for computers %1...").arg(where));
    } else {
        QString description = QStringLiteral("Computers %1 that share their engine.").arg(where);
        if (tailnet && !status->owner.empty()) {
            description += QStringLiteral(" Signed in to Tailscale as %1.").arg(q(status->owner));
        }
        findTile_->setDescription(description);
    }
    static const std::vector<remote::NearbyHost> none;
    const std::vector<remote::NearbyHost>& hosts = scan_ ? scan_->hosts : none;
    sweepButton_->setVisible(scan_ && hosts.empty() && !scan_->swept);
    for (const remote::NearbyHost& host : hosts) {
        findLayout_->addWidget(findRow(host));
    }
    if (!scan_ || !hosts.empty() || scanBusy_) {
        return;
    }
    findNote(QStringLiteral("None found. On the computer you want to use, open Settings → Remote "
                            "engine → Share this computer and turn on sharing.") +
             (scan_->swept ? QStringLiteral(" Checked every address on this network as well.")
                           : QStringLiteral(" If it's on, your router may be hiding it from the "
                                            "search: try Check every address, or enter its "
                                            "address.")));
    if (status && !tailnet) {
        QString reason;
        if (status->state == "stopped") {
            reason = QStringLiteral("Tailscale is installed but turned off; turn it on to find "
                                    "your computers on other networks too.");
        } else if (status->state == "needs_login") {
            reason = QStringLiteral("Tailscale is installed but not signed in; sign in to find "
                                    "your computers on other networks too.");
        } else if (status->state == "starting") {
            reason = QStringLiteral("Tailscale is still starting.");
        }
        if (reason.isEmpty() && status->state == "not_installed") {
            findNote(QStringLiteral("On a different network? %1 on both computers and sign in "
                                    "with the same account.")
                         .arg(tailscaleLink(QStringLiteral("Get Tailscale"))),
                     true);
        } else if (!reason.isEmpty()) {
            findNote(reason);
        }
    }
}

QWidget* RemoteEnginePage::namedControl(std::string_view name) {
    return name == "remote_publish_history" ? publishHistory_ : nullptr;
}

QWidget* RemoteEnginePage::findRow(const remote::NearbyHost& host) {
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("remoteFindRow"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    auto* label = new WrappedLabel(q(remote_text::find_row_detail(host)));
    label->setObjectName(QStringLiteral("remoteFindLabel"));
    label->setToolTip(q(host.address()));
    rowLayout->addWidget(label, 1);
    Button* button = nullptr;
    if (remote_text::find_row_connects(host)) {
        button = compactPrimaryButton(new PrimaryButton(QStringLiteral("Connect")));
        button->setObjectName(QStringLiteral("remoteFindConnectButton"));
        connect(button, &QPushButton::clicked, this, [this, host] { connectHost(host); });
    } else {
        button = neutralButton(new Button(QStringLiteral("Use a code")));
        button->setObjectName(QStringLiteral("remoteFindCodeButton"));
        connect(button, &QPushButton::clicked, this, [this, host] { useCode(host); });
    }
    button->setEnabled(host.compatible && !pairingBusy_);
    rowLayout->addWidget(button);
    return row;
}

void RemoteEnginePage::renderHost() {
    const remote::HostState& state = snapshot_.host;
    syncHostSwitches(state);

    const bool running = state.running;
    const std::optional<net::TailscaleStatus>& tailnet = state.tailscale;
    const bool onTailnet = tailnet && tailnet->running() && !tailnet->address.empty();
    const QString status = q(remote_text::sharing_status(
        state, remote_text::sharing_address(state, tailnet, onTailnet)));
    hostStatus_->setText(status);
    hostStatus_->setVisible(!status.isEmpty());
    if (running && !state.fingerprint.empty()) {
        hostIdentity_->setText(QStringLiteral("This computer's identity: %1. A computer that pairs "
                                              "should show the same.")
                                   .arg(q(remote::short_fingerprint(state.fingerprint))));
        hostIdentity_->show();
    } else {
        hostIdentity_->hide();
    }
    showHostNotes(running ? state.notes : std::vector<remote::Note>{});

    showTailscaleTrust(state, onTailnet);

    pairDeviceButton_->setEnabled(running);
    showPairing(running ? state.pairing : std::nullopt);
    std::vector<std::string> connected;
    for (const remote::ConnectedClient& client : state.clients) {
        connected.push_back(client.device_id);
    }
    rebuildDevices(state.devices, connected);
    recoverRecordsButton_->setEnabled(!recoveryBusy_);

    const std::optional<remote::ClientPairing>& pairing = snapshot_.pairing;
    QString rail;
    if (running) {
        rail = QStringLiteral("Sharing · %1 paired").arg(state.devices.size());
    } else if (pairing) {
        rail = QStringLiteral("Paired · %1").arg(q(pairing->host_name));
    } else {
        rail = QStringLiteral("Off");
    }
    railValue_ = rail;
    if (services_.setRailValue) {
        services_.setRailValue(rail);
    }
    refreshTabs(state, running);
}

void RemoteEnginePage::syncHostSwitches(const remote::HostState& state) {
    mirrorSwitch(shareTile_, state.enabled);
    const int port = state.port != 0 ? state.port : remote::DEFAULT_PORT;
    // Only a new saved port is shown: one being typed is saved once the field settles, and a
    // refresh meanwhile must not put the old one back.
    if (!savedPort_ || port != *savedPort_) {
        savedPort_ = port;
        const bool blocked = portSpin_->blockSignals(true);
        portSpin_->setValue(port);
        portSpin_->blockSignals(blocked);
    }
    mirrorSwitch(managementTile_, state.model_management);
    mirrorSwitch(keepRecordsTile_, state.keep_records);
    mirrorSwitch(manageMcpTile_, state.manage_mcp);
}

void RemoteEnginePage::showTailscaleTrust(const remote::HostState& state, bool onTailnet) {
    // Only offered where it can work: Tailscale running under a person's account (tagged
    // servers have no owner to compare against).
    const bool offered = onTailnet && !state.tailscale->owner.empty();
    tailscaleTile_->setVisible(offered);
    if (!offered) {
        return;
    }
    mirrorSwitch(tailscaleTile_, state.tailscale_trust);
    tailscaleTile_->setDescription(
        QStringLiteral("Computers signed in to Tailscale as %1 can pair by picking this one from "
                       "their tailnet. Anyone else on your tailnet still asks first, and you allow "
                       "it here.")
            .arg(q(state.tailscale->owner)));
}

void RemoteEnginePage::refreshTabs(const remote::HostState& state, bool running) {
    const std::optional<remote::ClientPairing>& pairing = snapshot_.pairing;
    tabs_->setDetail(0, pairing ? QStringLiteral("Paired · %1").arg(q(pairing->host_name))
                                : QStringLiteral("Not paired"));
    tabs_->setDetail(1, running ? QStringLiteral("Sharing · %1 paired").arg(state.devices.size())
                        : state.enabled ? QStringLiteral("Starting…")
                                        : QStringLiteral("Off"));
    if (!tabChosen_ && !pairing && (running || state.enabled)) {
        // Sharing and not using another computer: the host side is the one in use.
        tabs_->setCurrentIndex(1);
    }
}

void RemoteEnginePage::showSection(int index) {
    clientContainer_->setVisible(index == 0);
    hostContainer_->setVisible(index == 1);
    if (index == 0 && service_ != nullptr) {
        // The page may have opened on the sharing tab; search on first look here.
        whenUnpaired([this] { scanNearby(); });
    }
}

void RemoteEnginePage::showHostNotes(const std::vector<remote::Note>& notes) {
    std::vector<std::vector<std::string>> shown;
    for (const remote::Note& note : notes) {
        shown.push_back({note.kind, note.message, note.action, note.url});
    }
    if (shown == hostNotesShown_) {
        return;
    }
    hostNotesShown_ = shown;
    clearRows(hostNotesLayout_, true);
    for (const remote::Note& note : notes) {
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("remoteHostNoteRow"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto* label = new WrappedLabel(q(note.message));
        label->setObjectName(QStringLiteral("remoteHostNote"));
        rowLayout->addWidget(label, 1);
        if (!note.action.empty() && !note.url.empty()) {
            Button* button = neutralButton(new Button(q(note.action)));
            button->setObjectName(QStringLiteral("remoteHostNoteButton"));
            const QString url = q(note.url);
            connect(button, &QPushButton::clicked, this, [this, url] { openUrl(url); });
            rowLayout->addWidget(button, 0, Qt::AlignTop);
        }
        hostNotesLayout_->addWidget(row);
    }
    hostNotes_->setVisible(!notes.empty());
}

void RemoteEnginePage::openUrl(const QString& url) {
    if (services_.openUrl) {
        services_.openUrl(QUrl(url));
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
}

void RemoteEnginePage::showPairing(const std::optional<std::pair<std::string, double>>& pairing) {
    // The code replaces the button while it is valid.
    pairDeviceButton_->setVisible(!pairing.has_value());
    if (!pairing) {
        pairingBox_->hide();
        countdown_->stop();
        return;
    }
    const QString code = q(pairing->first);
    pairingCodeLabel_->setText(QStringLiteral("%1 %2").arg(code.left(3), code.mid(3)));
    // divmod(int(left), 60): int() truncates toward zero.
    const auto left = static_cast<long long>(pairing->second);
    long long minutes = left / 60;
    long long seconds = left % 60;
    if (seconds < 0) {
        seconds += 60;
        minutes -= 1;
    }
    pairingExpiryLabel_->setText(
        QStringLiteral("Enter this code on the other computer. Expires in %1:%2.")
            .arg(minutes)
            .arg(seconds, 2, 10, QLatin1Char('0')));
    pairingBox_->show();
    if (!countdown_->isActive()) {
        countdown_->start();
    }
}

void RemoteEnginePage::refreshPairingCode() {
    if (service_ == nullptr) {
        countdown_->stop();
        return;
    }
    run(
        countdownGate_, [service = service_] { return std::make_optional(service->host_state()); },
        [this](std::optional<remote::HostState> state, std::exception_ptr error) {
            if (error) {
                kLog.warning("Reading the pairing code failed: {}", toUtf8(errorText(error)));
                return;
            }
            // Only the code and whether sharing runs: the rest of this read may be older than a
            // switch the person moved since, which the next redraw would put back.
            snapshot_.host.running = state->running;
            snapshot_.host.pairing = std::move(state->pairing);
            showPairing(snapshot_.host.running ? snapshot_.host.pairing : std::nullopt);
        });
}

void RemoteEnginePage::rebuildDevices(const std::vector<remote::PairedDevice>& devices,
                                      const std::vector<std::string>& connected) {
    clearRows(devicesLayout_, false);
    if (devices.empty()) {
        auto* empty = new WrappedLabel(QStringLiteral("No computers are paired yet."));
        empty->setObjectName(QStringLiteral("infoLabel"));
        devicesLayout_->addWidget(empty);
        return;
    }
    for (const remote::PairedDevice& device : devices) {
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("remoteDeviceRow"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        QStringList details = {device.name.empty() ? QStringLiteral("Unnamed computer")
                                                   : q(device.name)};
        if (std::find(connected.begin(), connected.end(), device.id) != connected.end()) {
            details.append(QStringLiteral("connected now"));
        }
        const QString paired = q(remote_text::paired_on(device.paired_at, *clock_->zone()));
        if (!paired.isEmpty()) {
            const QString asName = device.paired_name.empty()
                                       ? QString()
                                       : QStringLiteral(" as %1").arg(q(device.paired_name));
            const QString via =
                device.via == "tailscale" ? QStringLiteral(" over Tailscale") : QString();
            details.append(QStringLiteral("paired %1%2%3").arg(paired, asName, via));
        }
        const auto records = snapshot_.deviceRecords.find(device.id);
        QString stored;
        if (records != snapshot_.deviceRecords.end()) {
            try {
                stored = q(remote_text::stored_phrase(records->second));
            } catch (const py::PyError& error) {
                kLog.debug("Could not count a paired computer's records: {}", error.what());
            }
        }
        if (!stored.isEmpty()) {
            details.append(QStringLiteral("keeps %1 here").arg(stored));
        }
        auto* label = new WrappedLabel(details.join(QStringLiteral(" · ")));
        label->setObjectName(QStringLiteral("remoteDeviceLabel"));
        Button* rename = neutralButton(new Button(QStringLiteral("Rename")));
        rename->setObjectName(QStringLiteral("remoteRenameDeviceButton"));
        rename->setToolTip(QStringLiteral("Change the name this computer shows for it"));
        connect(rename, &QPushButton::clicked, this, [this, device] { renameDevice(device); });
        auto* remove = new DangerButton(QStringLiteral("Remove"));
        // Not setObjectName: the theme styles it by "dangerButton".
        ui::setStyleProperty(remove, ui::prop::buttonId,
                             QStringLiteral("remoteRemoveDeviceButton"));
        connect(remove, &QPushButton::clicked, this, [this, id = device.id] { removeDevice(id); });
        rowLayout->addWidget(label, 1);
        rowLayout->addWidget(rename);
        rowLayout->addWidget(remove);
        devicesLayout_->addWidget(row);
    }
}

void RemoteEnginePage::whenUnpaired(std::function<void()> then) {
    run(
        unpairedGate_, [service = service_] { return service->client_pairing().has_value(); },
        [then = std::move(then)](bool paired, std::exception_ptr error) {
            if (error) {
                kLog.warning("Reading the pairing failed: {}", toUtf8(errorText(error)));
                return;
            }
            if (!paired) {
                then();
            }
        });
}

// ---- finding a computer ----

void RemoteEnginePage::onShown() {
    if (service_ == nullptr) {
        return;
    }
    run(
        unpairedGate_,
        [service = service_, records = records_] {
            const bool paired = service->client_pairing().has_value();
            if (paired && records != nullptr) {
                try {
                    records->refresh_summary();
                } catch (const std::exception& error) {
                    kLog.warning("Asking the host what it keeps failed: {}", error.what());
                }
            }
            return paired;
        },
        [this](bool paired, std::exception_ptr error) {
            if (error) {
                kLog.warning("Reading the pairing failed: {}", toUtf8(errorText(error)));
                return;
            }
            if (paired) {
                return;
            }
            if (!scanAt_ ||
                now() - *scanAt_ > std::chrono::duration<double>(remote_text::TAILNET_RESCAN_S)) {
                scanNearby();
            }
        });
}

void RemoteEnginePage::scanNearby(bool force, bool sweep) {
    if (service_ == nullptr || scanBusy_) {
        return;
    }
    if (!force && scanAt_ &&
        now() - *scanAt_ <= std::chrono::duration<double>(remote_text::TAILNET_RESCAN_S)) {
        return;
    }
    scanBusy_ = true;
    sweeping_ = sweep;
    run(
        scanGate_,
        [service = service_, sweep] { return std::make_optional(service->scan_nearby(sweep)); },
        [this](std::optional<remote::NearbyScan> scan, std::exception_ptr error) {
            if (error) {
                kLog.warning("Looking for computers to pair with failed: {}",
                             toUtf8(errorText(error)));
            }
            scanBusy_ = false;
            sweeping_ = false;
            scanAt_ = now();
            scan_ = error ? std::nullopt : std::move(scan);
            refresh();
        });
    refresh();
}

// ---- actions ----

void RemoteEnginePage::say(const QString& text) {
    // One line in each state's tile, so a message survives the switch between finding a
    // computer and being paired with one.
    clientMessage_->setText(text);
    findMessage_->setText(text);
}

void RemoteEnginePage::showManual() {
    manual_ = true;
    refresh();
    addressEdit_->setFocus();
}

void RemoteEnginePage::pair() {
    if (service_ == nullptr || pairingBusy_) {
        return;
    }
    const QString address = addressEdit_->text();
    const QString code = codeEdit_->text();
    try {
        remote::parse_address(toUtf8(address));
    } catch (const py::ValueError& error) {
        say(toQString(error.what()));
        return;
    }
    if (!hasDigit(code)) {
        say(QStringLiteral("Enter the pairing code shown on the host."));
        return;
    }
    startPairing(address, code, false, std::nullopt);
}

void RemoteEnginePage::startPairing(const QString& address, std::optional<QString> code,
                                    bool tailscale, std::optional<remote::NearbyHost> host) {
    pairingBusy_ = true;
    pairButton_->setEnabled(false);
    say(QStringLiteral("Pairing..."));
    run(
        pairGate_,
        [service = service_, address = toUtf8(address),
         code = code ? std::make_optional(toUtf8(*code)) : std::nullopt, tailscale] {
            return std::make_optional(service->pair(address, code.value_or(""), tailscale));
        },
        [this, host = std::move(host)](std::optional<remote::ClientPairing> pairing,
                                       std::exception_ptr error) {
            if (error) {
                kLog.warning("Pairing with a host failed: {}", toUtf8(errorText(error)));
            }
            onPairFinished(error ? std::nullopt : std::move(pairing), error, host);
        });
    refresh();
}

void RemoteEnginePage::connectHost(const remote::NearbyHost& host) {
    if (service_ == nullptr || pairingBusy_) {
        return;
    }
    if (host.can_pair_without_code()) {
        startPairing(q(host.address()), std::nullopt, true, host);
    } else {
        requestPairing(host);
    }
}

void RemoteEnginePage::retryPairingSave() {
    if (service_ == nullptr || pairingBusy_) {
        return;
    }
    pairingBusy_ = true;
    say(QStringLiteral("Requesting access to save the connection key..."));
    run(
        pairGate_,
        [service = service_] { return std::make_optional(service->retry_pairing_save()); },
        [this](std::optional<remote::ClientPairing> pairing, std::exception_ptr error) {
            if (error) {
                kLog.warning("Saving the pairing key failed: {}", toUtf8(errorText(error)));
            }
            onPairFinished(error ? std::nullopt : std::move(pairing), error, std::nullopt);
        });
    refresh();
}

void RemoteEnginePage::requestPairing(const remote::NearbyHost& host) {
    CancelSource cancel;
    pairingBusy_ = true;
    const QString name = q(host.name());
    request_ = Request{name, QString(), cancel};
    pairButton_->setEnabled(false);
    say(QStringLiteral("Asking %1...").arg(name));
    const std::uint64_t token = pairGate_.next();
    qt::TokenGate gate = pairGate_;
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    services_.io->post([this, ui, gate, token, service = service_, host, cancel] {
        std::optional<remote::ClientPairing> pairing;
        std::exception_ptr error;
        try {
            pairing = service->request_pairing(
                host.address(),
                [this, ui, gate, token](std::string sas) {
                    ui->post([this, gate, token, sas = std::move(sas)] {
                        if (gate.isCurrent(token)) {
                            onPairCode(q(sas));
                        }
                    });
                },
                cancel.token());
        } catch (const remote::PairingCanceled&) {
            kLog.debug("Pairing request canceled");
            error = std::current_exception();
        } catch (const std::exception& caught) {
            kLog.warning("Requesting to pair with a host failed: {}", caught.what());
            error = std::current_exception();
        } catch (...) {
            error = std::current_exception();
        }
        ui->post([this, gate, token, pairing, error, host] {
            if (gate.isCurrent(token)) {
                onPairFinished(pairing, error, host);
            }
        });
    });
    refresh();
}

void RemoteEnginePage::onPairCode(const QString& sas) {
    if (!request_) {
        return;
    }
    request_->sas = sas;
    const QString name = request_->name;
    waitCodeLabel_->setText(q(remote::format_sas(toUtf8(sas))));
    waitLabel_->setText(QStringLiteral("On %1, click Allow if it shows this number. If it shows a "
                                       "different one, click Deny there: something else is "
                                       "answering for it.")
                            .arg(name));
    say(QStringLiteral("Waiting for %1 to allow this computer...").arg(name));
    refresh();
}

void RemoteEnginePage::cancelRequest() {
    if (request_) {
        request_->cancel.cancel();
        say(QStringLiteral("Canceling..."));
    }
}

void RemoteEnginePage::useCode(const remote::NearbyHost& host) {
    manual_ = true;
    addressEdit_->setText(q(host.address()));
    refresh();
    codeEdit_->setFocus();
    say(QStringLiteral("On %1, click \"Show a pairing code\" under Settings → Remote engine → "
                       "Share this computer (\"Pair a device\" in older versions), then enter the "
                       "code here and click Pair.")
            .arg(q(host.name())));
}

QString RemoteEnginePage::pairErrorText(std::exception_ptr error,
                                        const std::optional<remote::NearbyHost>& host) {
    try {
        std::rethrow_exception(error);
    } catch (const CredentialStoreError&) {
        return QStringLiteral(
            "Couldn't save the connection key. Retry access below to finish pairing.");
    } catch (const remote::PairingCanceled&) {
        return QStringLiteral("Pairing canceled.");
    } catch (const remote::RemoteUnreachable& unreachable) {
        if (host && host->lan && !unreachable.refused) {
            // It answered the search, so it is on this network; the connection itself is what's
            // being stopped.
            const QString name = q(host->name());
            return QStringLiteral("%1 answered the search but didn't accept the connection. Its "
                                  "firewall may be blocking %2: on %1, open Settings → Remote "
                                  "engine → Share this computer to see what to change.")
                .arg(name, q(identity().product));
        }
        return errorText(std::current_exception());
    } catch (...) {
        return errorText(std::current_exception());
    }
}

void RemoteEnginePage::onPairFinished(std::optional<remote::ClientPairing> pairing,
                                      std::exception_ptr error,
                                      std::optional<remote::NearbyHost> host) {
    pairingBusy_ = false;
    request_.reset();
    waitBox_->hide();
    pairButton_->setEnabled(true);
    if (error || !pairing) {
        say(error ? pairErrorText(error, host) : QStringLiteral("Exception"));
    } else if (pairing->via == "tailscale") {
        say(QStringLiteral("Paired with %1 through your Tailscale account. Click \"Use for "
                           "dictation\" to start using it.")
                .arg(q(pairing->host_name)));
    } else if (pairing->via == "approval") {
        manual_ = false;
        say(QStringLiteral("Paired with %1. Click \"Use for dictation\" to start using it.")
                .arg(q(pairing->host_name)));
    } else {
        manual_ = false;
        codeEdit_->clear();
        say(QStringLiteral("Paired with %1. Check that its identity, %2, matches the one shown "
                           "there.")
                .arg(q(pairing->host_name), q(remote::short_fingerprint(pairing->fingerprint))));
    }
    if (pairing) {
        // Drawn from what pairing returned until the next read arrives.
        snapshot_.pairing = pairing;
    }
    discardReads();
    refresh();
}

void RemoteEnginePage::useEngine() {
    // OpenWhisper says so only when the controller's callback is there to switch the engine.
    if (!isSignalConnected(QMetaMethod::fromSignal(&RemoteEnginePage::useForDictationRequested))) {
        return;
    }
    Q_EMIT useForDictationRequested();
    if (snapshot_.pairing) {
        say(QStringLiteral("Dictation now uses %1's engine.").arg(q(snapshot_.pairing->host_name)));
    }
}

void RemoteEnginePage::useForMeetings() {
    Q_EMIT useForMeetingsRequested();
    Q_EMIT settingsDestinationRequested(QStringLiteral("meeting_voice"));
}

void RemoteEnginePage::manageModels() {
    if (service_ == nullptr || !snapshot_.pairing) {
        return;
    }
    Q_EMIT manageHostModelsRequested(q(snapshot_.pairing->host_name),
                                     q(snapshot_.pairing->fingerprint));
}

void RemoteEnginePage::renameHost() {
    if (service_ == nullptr || !snapshot_.pairing) {
        return;
    }
    const remote::ClientPairing& pairing = *snapshot_.pairing;
    const QString own = q(pairing.paired_name.empty() ? pairing.host_name : pairing.paired_name);
    const std::optional<QString> name = services_.prompts.askName(
        clientTile_->window(), QStringLiteral("Rename host"),
        QStringLiteral("The name this computer shows for its host. Leave it empty to go back to "
                       "%1, the name it gave when pairing.")
            .arg(own),
        q(pairing.host_name), own);
    if (!name) {
        return;
    }
    run([service = service_, name = toUtf8(*name)] { return service->rename_host(name); },
        [this](std::optional<remote::ClientPairing> renamed, std::exception_ptr error) {
            if (error) {
                kLog.warning("Could not rename the remote host: {}", toUtf8(errorText(error)));
                services_.prompts.warning(clientTile_->window(),
                                          QStringLiteral("Could not rename host"),
                                          errorText(error));
                return;
            }
            discardReads();
            if (renamed) {
                snapshot_.pairing = renamed;
                say(QStringLiteral("This computer now calls the host %1.")
                        .arg(q(renamed->host_name)));
            }
            refresh();
        });
}

void RemoteEnginePage::forget() {
    if (service_ == nullptr) {
        return;
    }
    const std::optional<remote::SyncStatus>& status = snapshot_.records;
    const QString stored = status ? q(remote_text::stored_phrase(status->stored)) : QString();
    if (!stored.isEmpty()) {
        const QString host =
            status->host_name.empty() ? QStringLiteral("the host") : q(status->host_name);
        if (!services_.prompts.question(
                clientTile_->window(), QStringLiteral("Forget host"),
                QStringLiteral("%1 keeps %2 of this computer's. Once you forget it, this computer "
                               "can't list or bring them back; they stay on %1. Forget it anyway?")
                    .arg(host, stored))) {
            return;
        }
    }
    run(
        [service = service_] {
            service->forget_host();
            return true;
        },
        [this](bool, std::exception_ptr error) {
            if (error) {
                kLog.warning("Could not forget the remote host: {}", toUtf8(errorText(error)));
                return;
            }
            discardReads();
            snapshot_.pairing.reset();
            say(QStringLiteral("Forgot the host. Pair again to use it."));
            scanNearby(true);
        });
}

void RemoteEnginePage::onShareToggled(bool checked) {
    if (service_ == nullptr) {
        return;
    }
    remote::RemoteEngineControl* service = service_;
    snapshot_.host.enabled = checked;
    applySharing([service, checked] {
        service->set_host_enabled(checked);
        if (checked) {
            const remote::HostState state = service->host_state();
            if (state.running && state.devices.empty() && !state.pairing) {
                // Someone turning sharing on is about to pair a computer; have a code ready for
                // one that types it, as well as for its request.
                service->open_pairing();
            }
        }
    });
}

void RemoteEnginePage::applySharing(std::function<void()> change) {
    // On io: stopping the server waits for connected computers' requests, and starting can
    // create the TLS certificate. The page refreshes even when it failed.
    discardReads();
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    runDetached([this, ui, change = std::move(change)] {
        try {
            change();
        } catch (const std::exception& error) {
            kLog.warning("Couldn't change sharing: {}", error.what());
        }
        ui->post([this] { onServiceEvent("state"); });
    });
}

void RemoteEnginePage::setPermission(std::function<void(remote::RemoteEngineControl&)> change,
                                     std::function<void(remote::HostState&)> shown) {
    if (service_ == nullptr) {
        return;
    }
    // What was just saved is drawn until the service says otherwise: a redraw from the state
    // read before would move the switch back.
    shown(snapshot_.host);
    discardReads();
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    runDetached([this, ui, service = service_, change = std::move(change)] {
        try {
            change(*service);
        } catch (const std::exception& error) {
            kLog.warning("Couldn't save a sharing permission: {}", error.what());
        }
        ui->post([this] { onServiceEvent("state"); });
    });
}

void RemoteEnginePage::discardReads() {
    // A read that started before a change would draw the state from before it over the switch
    // the person just moved; the read after the change is the one that counts.
    snapshotGate_.next();
    recordsGate_.next();
    countdownGate_.next();
}

// ---- where records are kept ----

void RemoteEnginePage::chooseLocation(const std::string& location) {
    if (records_ == nullptr) {
        return;
    }
    if (records_->location() == location) {
        return;
    }
    if (snapshot_.records) {
        snapshot_.records->location = location;
    }
    discardReads();
    run(
        locationGate_,
        [records = records_, location] {
            records->set_location(location);
            return std::make_optional(records->status());
        },
        [this, location](std::optional<remote::SyncStatus> status, std::exception_ptr error) {
            if (error) {
                kLog.warning("Couldn't change where records are kept: {}",
                             toUtf8(errorText(error)));
                recordsNote_ = QStringLiteral("Couldn't change where records are kept: %1")
                                   .arg(errorText(error));
            } else {
                const QString host =
                    status->host_name.empty() ? QStringLiteral("the host") : q(status->host_name);
                if (location == "local") {
                    recordsNote_ = QStringLiteral("New records stay here. Those already on %1 stay "
                                                  "there until you bring them back.")
                                       .arg(host);
                } else if (records_->manual_transfers()) {
                    recordsNote_ = QStringLiteral("Choose the transfer button when you are ready.");
                } else if (location == "host") {
                    recordsNote_ = QStringLiteral("New records will move to %1.").arg(host);
                } else {
                    recordsNote_ =
                        QStringLiteral("New records will also be copied to %1.").arg(host);
                }
                snapshot_.records = std::move(status);
            }
            renderRecords();
            requestSnapshot();
        });
}

void RemoteEnginePage::runRecordsJob(const QString& busy, std::function<QString()> work) {
    if (!recordsBusy_.isEmpty()) {
        return;
    }
    recordsBusy_ = busy;
    recordsNote_.clear();
    renderRecords();
    run(
        recordsJobGate_, [work = std::move(work)] { return work(); },
        [this](QString message, std::exception_ptr error) {
            if (error) {
                kLog.warning("Remote records job failed: {}", toUtf8(errorText(error)));
                message = errorText(error);
            }
            recordsBusy_.clear();
            recordsNote_ = message;
            requestRecords();
        });
}

void RemoteEnginePage::onRecordsEvent(const std::string& kind) {
    constexpr std::string_view progress = "progress:";
    if (kind.starts_with(progress) && !recordsBusy_.isEmpty()) {
        recordsBusy_ = q(std::string_view(kind).substr(progress.size()));
    }
    requestRecords();
}

void RemoteEnginePage::sendExisting() {
    if (records_ == nullptr) {
        return;
    }
    if (records_->manual_transfers() && snapshot_.records) {
        const bool move = snapshot_.records->location == "host";
        const auto host = q(snapshot_.records->host_name);
        const auto question =
            move ? QStringLiteral("Move the existing dictations and their recordings to %1? "
                                  "Local copies are removed only after the host confirms storage.")
                       .arg(host)
                 : QStringLiteral("Copy the existing dictations and their recordings to %1?")
                       .arg(host);
        if (!services_.prompts.question(storageTile_->window(), sendExistingButton_->text(),
                                        question)) {
            return;
        }
    }
    runRecordsJob(QStringLiteral("Queuing records…"), [records = records_] {
        const int queued = records->send_existing();
        if (queued == 0) {
            return QStringLiteral("Everything here is already on the host.");
        }
        return QStringLiteral("Queued %1 record%2 to send.")
            .arg(queued)
            .arg(queued != 1 ? QStringLiteral("s") : QString());
    });
}

void RemoteEnginePage::bringBack() {
    if (records_ == nullptr || !snapshot_.records) {
        return;
    }
    const remote::SyncStatus& status = *snapshot_.records;
    const QString host =
        status.host_name.empty() ? QStringLiteral("the host") : q(status.host_name);
    const bool toLocal = status.location != "local";
    if (toLocal &&
        !services_.prompts.question(storageTile_->window(), QStringLiteral("Bring records back"),
                                    QStringLiteral("Bring every record on %1 back to this "
                                                   "computer, and keep new ones here from now on?")
                                        .arg(host))) {
        return;
    }
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    runRecordsJob(
        QStringLiteral("Bringing records back from %1…").arg(host),
        [this, ui, records = records_, host, toLocal] {
            if (toLocal) {
                records->set_location("local");
            }
            const int moved = records->bring_back([this, ui](std::string text) {
                ui->post([this, text = std::move(text)] { onRecordsEvent("progress:" + text); });
            });
            return QStringLiteral("Brought %1 record%2 back from %3.")
                .arg(moved)
                .arg(moved != 1 ? QStringLiteral("s") : QString(), host);
        });
}

void RemoteEnginePage::onPortChanged() {
    // Each arrow click is a new port, and each one restarts the server.
    portTimer_->start();
}

void RemoteEnginePage::applyPort() {
    if (service_ == nullptr) {
        return;
    }
    const int port = portSpin_->value();
    applySharing([service = service_, port] { service->set_host_port(port); });
}

void RemoteEnginePage::openPairing() {
    if (service_ == nullptr) {
        return;
    }
    applySharing([service = service_] { service->open_pairing(); });
}

void RemoteEnginePage::cancelPairing() {
    if (service_ == nullptr) {
        return;
    }
    applySharing([service = service_] { service->cancel_pairing(); });
}

void RemoteEnginePage::renameDevice(const remote::PairedDevice& device) {
    if (service_ == nullptr || device.id.empty()) {
        return;
    }
    const QString current = q(device.name);
    const QString own = device.paired_name.empty() ? current : q(device.paired_name);
    const std::optional<QString> name = services_.prompts.askName(
        devicesTile_->window(), QStringLiteral("Rename paired computer"),
        QStringLiteral("The name this computer shows for it here, on the host dashboard and in "
                       "History. Leave it empty to go back to %1, the name it paired with.")
            .arg(own),
        current, own);
    if (!name) {
        return;
    }
    run(
        [service = service_, id = device.id, name = toUtf8(*name)] {
            service->rename_device(id, name);
            return true;
        },
        [this](bool, std::exception_ptr error) {
            if (error) {
                kLog.warning("Could not rename a paired computer: {}", toUtf8(errorText(error)));
                services_.prompts.warning(devicesTile_->window(),
                                          QStringLiteral("Could not rename computer"),
                                          errorText(error));
            }
            refresh();
        });
}

void RemoteEnginePage::removeDevice(const std::string& deviceId) {
    if (service_ == nullptr || deviceId.empty()) {
        return;
    }
    QString stored;
    if (const auto found = snapshot_.deviceRecords.find(deviceId);
        found != snapshot_.deviceRecords.end()) {
        try {
            stored = q(remote_text::stored_phrase(found->second));
        } catch (const py::PyError&) {
        }
    }
    bool deleteRecords = false;
    if (!stored.isEmpty()) {
        QString name;
        for (const remote::PairedDevice& device : snapshot_.host.devices) {
            if (device.id == deviceId) {
                name = q(device.name);
                break;
            }
        }
        if (name.isEmpty()) {
            name = QStringLiteral("This computer");
        }
        const auto choice = services_.prompts.removeDevice(devicesTile_->window(), name, stored);
        if (choice == RemoteEnginePrompts::RemoveChoice::Cancel) {
            return;
        }
        deleteRecords = choice == RemoteEnginePrompts::RemoveChoice::DeleteRecords;
    }
    run([service = service_, deviceId,
         deleteRecords] { return service->remove_device(deviceId, deleteRecords); },
        [this](bool, std::exception_ptr error) {
            if (error) {
                kLog.warning("Could not remove a paired computer: {}", toUtf8(errorText(error)));
                services_.prompts.warning(devicesTile_->window(),
                                          QStringLiteral("Could not remove computer"),
                                          errorText(error));
            }
            refresh();
        });
}

void RemoteEnginePage::recoverRecords() {
    if (service_ == nullptr || recoveryBusy_) {
        return;
    }
    using Loaded = std::optional<
        std::pair<std::vector<remote::RecordOwnerGroup>, std::vector<remote::PairedDevice>>>;
    run(
        recoveryGate_,
        [service = service_]() -> Loaded {
            auto owners = service->recoverable_record_owners();
            auto devices = service->host_state().devices;
            return std::make_pair(std::move(owners), std::move(devices));
        },
        [this](Loaded loaded, std::exception_ptr error) {
            QWidget* window = devicesTile_->window();
            if (error) {
                kLog.warning("Could not load stored records to recover: {}",
                             toUtf8(errorText(error)));
                services_.prompts.warning(window, QStringLiteral("Could not load stored records"),
                                          errorText(error));
                return;
            }
            const auto& [owners, devices] = *loaded;
            if (owners.empty() || devices.empty()) {
                services_.prompts.information(
                    window, QStringLiteral("Recover stored records"),
                    devices.empty()
                        ? QStringLiteral(
                              "Pair the original computer again, then assign its old records here.")
                        : QStringLiteral("No records from unpaired computers need recovery."));
                return;
            }
            const auto choice = services_.prompts.recoverRecords(window, owners, devices);
            if (!choice || recoveryBusy_) {
                return;
            }
            recoveryBusy_ = true;
            recoveryMessage_->setText(QStringLiteral("Recovering stored records…"));
            recoveryMessage_->show();
            refresh();
            run(
                recoveryGate_,
                [service = service_, owner = choice->first, device = choice->second] {
                    try {
                        service->recover_device_records(owner, device);
                        return QStringLiteral("Stored records are available to the paired "
                                              "computer. Refresh its history to see them.");
                    } catch (const std::exception& failure) {
                        kLog.warning("Could not recover a paired computer's records: {}",
                                     failure.what());
                        return QStringLiteral("Could not recover records: %1")
                            .arg(toQString(failure.what()));
                    }
                },
                [this](QString message, std::exception_ptr) {
                    recoveryBusy_ = false;
                    recoveryMessage_->setText(message);
                    recoveryMessage_->show();
                    refresh();
                });
        });
}

void RemoteEnginePage::onServiceEvent(const std::string& kind) {
    // "activity" fires around every request a paired computer makes, and nothing on this page
    // shows it.
    if (kind != "activity") {
        refresh();
    }
}

} // namespace fv
