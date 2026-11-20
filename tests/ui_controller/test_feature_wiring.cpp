// "controller connects every signal" (architecture 8.5): once the controller is installed, every
// signal of the main window, the tray, the overlay, the Quick Record and Upload File tabs, the
// compact controller and the History panel has a receiver, or is listed below with the
// milestone that connects it or why nothing does.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <QMetaMethod>

#include "controller_support.h"
#include "ui_history/history_feature.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/record_feature.h"
#include "ui_record/transcription_tab_base.h"
#include "ui_record/upload_file_tab.h"
#include "ui_widgets/tabbed_content.h"

using fvtest::ControllerRig;

namespace {

using Unused = std::map<std::string, std::string>;

const Unused kWindow{
    {"walkthroughRequested", "M6"},
    {"walkthroughLibraryRequested", "M6"},
    {"walkthroughsChanged", "M6"},
    {"remoteModelSelected", "M9"},
    {"remoteRuntimeSelected", "M9"},
    {"remoteRetryRequested", "M9"},
    {"livePreviewChanged", "no emitter: the controller takes each tab's livePreviewChanged"},
    {"uploadFileRequested", "no emitter: UploadPresenter takes the Upload tab's requests"},
    {"uploadFilesRequested", "no emitter: UploadPresenter takes the Upload tab's requests"},
    {"uploadCancelRequested", "no emitter: UploadPresenter takes the Upload tab's requests"},
    {"uploadCopyRequested", "no emitter: UploadPresenter takes the Upload tab's requests"},
    {"meetingDashboardRequested", "M7"},
    {"hostModelSelected", "M9"},
    {"pastMeetingRequested", "M7"},
    {"pastMeetingCopyRequested", "M7"},
    {"pastMeetingDeleteRequested", "M7"},
    {"pastMeetingsClearRequested", "M7"},
    {"compactModeChanged", "no consumer: OpenWhisper's window had no such signal"},
};

const Unused kTray{
    {"meetingToggleRequested", "M7"},
    {"meetingDashboardRequested", "M7"},
    {"walkthroughRequested", "M6"},
    {"screenshotRequested", "M6"},
    {"statsRequested", "History opens Stats from the tray's menu action itself"},
};

const Unused kOverlay{
    {"stateChanged", "no consumer: the overlay's own trace"},
    {"walkthroughSnapRequested", "M6"},
    {"walkthroughFinishRequested", "M6"},
};

const Unused kQuickRecord{};

const Unused kUpload{};

const Unused kCompact{};

const Unused kHistory{
    {"entryCopied", "M1.15's presenter shows its transient status itself"},
    {"entryDeleted", "M1.15's presenter shows its transient status itself"},
    {"exportRequested", "no consumer: HistoryPresenter takes the sidebar's request itself"},
    {"calendarRequested", "no consumer: HistoryPresenter takes the sidebar's request itself"},
};
// QObject::isSignalConnected is protected; a member pointer taken through a derived class's
// using-declaration names QObject's own member, which any QObject can be asked.
struct SignalProbe : QObject {
    using QObject::isSignalConnected;
};

bool connected(const QObject& object, const QMetaMethod& signal) {
    return (object.*(&SignalProbe::isSignalConnected))(signal);
}

// The signals declared from `first` (a class's own, or its own and a base's) down.
std::vector<QMetaMethod> signalsFrom(const QMetaObject& meta, const QMetaObject& first) {
    std::vector<QMetaMethod> found;
    for (int i = first.methodOffset(); i < meta.methodCount(); ++i) {
        if (meta.method(i).methodType() == QMetaMethod::Signal) {
            found.push_back(meta.method(i));
        }
    }
    return found;
}

void checkConnected(const QObject& object, const QMetaObject& first, const Unused& unused) {
    std::set<std::string> declared;
    for (const QMetaMethod& signal : signalsFrom(*object.metaObject(), first)) {
        const std::string name = signal.name().toStdString();
        declared.insert(name);
        INFO(object.metaObject()->className() << "::" << name);
        if (unused.contains(name)) {
            CHECK_FALSE(connected(object, signal));
        } else {
            CHECK(connected(object, signal));
        }
    }
    for (const auto& [name, owner] : unused) {
        INFO(name << " (" << owner << ")");
        CHECK(declared.contains(name));
    }
}

} // namespace

TEST_CASE("controller connects every signal", "[widget]") {
    ControllerRig rig;
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    auto& record = rig.context().service<fv::RecordWidgets>();
    fv::UploadFileTab* upload = qobject_cast<fv::UploadFileTab*>(rig.context().screens().ensure(
        fv::RecordFeature::kUploadScreen, rig.window().tabbedContent()->stack()));
    REQUIRE(upload == record.existingUpload());
    auto& panel = rig.context().service<fv::HistoryPanel>();

    checkConnected(rig.window(), fv::MainWindow::staticMetaObject, kWindow);
    checkConnected(rig.tray(), fv::SystemTray::staticMetaObject, kTray);
    checkConnected(rig.overlay(), fv::WaveformOverlay::staticMetaObject, kOverlay);
    checkConnected(*tab, fv::TranscriptionTabBase::staticMetaObject, kQuickRecord);
    checkConnected(*upload, fv::TranscriptionTabBase::staticMetaObject, kUpload);
    checkConnected(*rig.ui().compactController(), fv::CompactRecordController::staticMetaObject,
                   kCompact);
    checkConnected(panel, fv::HistoryPanel::staticMetaObject, kHistory);
}
