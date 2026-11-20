// The status a failed clipboard restore leaves (ui_controller.py _on_clipboard_restore_failed,
// docs/port/map/a06-hotkeys-paste-focus.md A06-37). The clipboard is a fake backend, never the
// system one.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

#include <QObject>
#include <QString>

#include "clipboard/temporary_clipboard.h"
#include "clipboard/testing/fake_backend.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "ui_controller/clipboard_service.h"

namespace {

struct ServiceRig {
    ServiceRig() {
        auto owned =
            std::make_unique<fv::clipboard::FakeClipboardBackend>(QStringLiteral("previous text"));
        backend = owned.get();
        temporary = std::make_shared<fv::clipboard::TemporaryClipboard>(
            std::move(owned), fv::clipboard::SequenceSource{}, fv::clipboard::Renderer{}, nullptr,
            true);
        service = std::make_unique<fv::ClipboardService>(
            temporary, std::make_shared<fv::focus::FakeSyntheticKeys>());
        QObject::connect(service.get(), &fv::ClipboardService::statusRequested,
                         [this](const QString& status) { statuses.push_back(status); });
    }

    fv::clipboard::FakeClipboardBackend* backend = nullptr;
    std::shared_ptr<fv::clipboard::TemporaryClipboard> temporary;
    std::unique_ptr<fv::ClipboardService> service;
    std::vector<QString> statuses;
};

} // namespace

TEST_CASE("a clipboard restore that fails leaves its status on the line", "[clipboard_service]") {
    ServiceRig rig;
    const auto stage = rig.service->stageTranscriptForPaste("dictated text");
    REQUIRE(stage.written);
    REQUIRE(stage.lease);
    CHECK(rig.statuses.empty());

    // The write that puts the user's clipboard back is refused.
    rig.backend->fail_writes = true;
    CHECK(rig.temporary->restore_now(*stage.lease) == fv::focus::RestoreOutcome::Failed);

    // Whatever the clipboard's own message was, the status is this one.
    CHECK(rig.statuses ==
          std::vector<QString>{QStringLiteral("Ready (Pasted; clipboard restore failed)")});
    CHECK(rig.backend->text() == QStringLiteral("dictated text"));
}

TEST_CASE("a clipboard restore that works asks for no status", "[clipboard_service]") {
    ServiceRig rig;
    const auto stage = rig.service->stageTranscriptForPaste("dictated text");
    REQUIRE(stage.lease);

    CHECK(rig.temporary->restore_now(*stage.lease) == fv::focus::RestoreOutcome::Restored);

    CHECK(rig.statuses.empty());
    CHECK(rig.backend->text() == QStringLiteral("previous text"));
}
