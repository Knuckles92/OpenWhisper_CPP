// The batch download confirmation and the component profile popup, and the feature that owns
// them (settings_downloads.py BatchDownloadDialog, component_details_dialog.py).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QLabel>
#include <QPushButton>
#include <QUrl>

#include "models/component_details.h"
#include "ui_downloads/batch_download_dialog.h"
#include "ui_downloads/component_details_dialog.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads_support.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/wrapped_label.h"

using fv::BatchDownloadDialog;
using fv::ComponentDetailsDialog;

namespace {

const QString kFolder = QStringLiteral("C:/Users/test/AppData/Local/OpenWhisper_CPP/speech-models");

QString dot(const QString& name, const QString& size) {
    return name + QString::fromUtf8("  \u00b7  ") + size;
}

} // namespace

TEST_CASE("the batch dialog lists each model with its size, the total and the destination",
          "[widget]") {
    fvtest::AppTheme theme;
    BatchDownloadDialog dialog({"parakeet-v3", "orukeet-v0.1"}, kFolder);

    CHECK(dialog.windowTitle() == "Download models");
    CHECK(dialog.objectName() == "batchDownloadDialog");
    CHECK(dialog.isModal());
    CHECK(dialog.minimumWidth() == 420);
    CHECK(dialog.titleLabel()->text() == "Download 2 models?");
    CHECK(dialog.modelListLabel()->text() ==
          dot("parakeet-v3", "~714 MB") + "\n" + dot("orukeet-v0.1", "~714 MB"));
    CHECK(dialog.modelListLabel()->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    // 714 MB twice, as the catalog estimates them.
    CHECK(dialog.totalLabel()->text() == "Estimated total: ~1.43 GB");
    CHECK(dialog.destinationPathLabel()->text() == kFolder);
    CHECK(dialog.destinationPathLabel()->toolTip() == kFolder);
    CHECK(fvtest::buttonText(dialog, "Download 2 models\u2026") != nullptr);
    CHECK(fvtest::buttonText(dialog, "Cancel") != nullptr);
    CHECK(dialog.findChild<QLabel*>("downloadsEyebrow")->text() == "DESTINATION");
    auto* note = dialog.findChild<fv::WrappedLabel*>("downloadsSourceNote");
    REQUIRE(note != nullptr);
    CHECK(note->text().startsWith("Models download one at a time, in the order listed."));
}

TEST_CASE("the batch dialog says model in the singular and unknown for a model with no estimate",
          "[widget]") {
    fvtest::AppTheme theme;
    BatchDownloadDialog one({"parakeet-v3"}, kFolder);
    CHECK(one.titleLabel()->text() == "Download 1 model?");
    CHECK(fvtest::buttonText(one, "Download 1 model\u2026") != nullptr);
    CHECK(one.totalLabel()->text() == "Estimated total: ~714 MB");

    BatchDownloadDialog unknown({"parakeet-v3", "someone/custom"}, kFolder);
    CHECK(unknown.totalLabel()->text() == "Estimated total: unknown");
    CHECK(unknown.modelListLabel()->text() ==
          dot("parakeet-v3", "~714 MB") + "\n" + dot("someone/custom", "size unknown"));
}

TEST_CASE("accepting the batch dialog is the consent and Cancel refuses it", "[widget]") {
    fvtest::AppTheme theme;
    {
        BatchDownloadDialog dialog({"parakeet-v3"}, kFolder);
        fvtest::buttonText(dialog, "Download 1 model\u2026")->click();
        CHECK(dialog.result() == QDialog::Accepted);
    }
    {
        BatchDownloadDialog dialog({"parakeet-v3"}, kFolder);
        fvtest::buttonText(dialog, "Cancel")->click();
        CHECK(dialog.result() == QDialog::Rejected);
    }
}

namespace {

fv::models::ComponentInfo cudaInfo() {
    fv::models::ComponentInfo info;
    info.component_id = "asr-nvidia-cuda";
    info.display_name = "NVIDIA Speech GPU";
    info.download_bytes = 117'000'000;
    info.install_bytes = 410'000'000;
    return info;
}

} // namespace

TEST_CASE("the component popup shows the bundled profile and the sizes", "[widget]") {
    fvtest::AppTheme theme;
    ComponentDetailsDialog dialog("asr-nvidia-cuda", cudaInfo());
    const auto& details = fv::models::get_component_details("asr-nvidia-cuda");

    CHECK(dialog.objectName() == "componentDetailsDialog");
    CHECK(dialog.windowTitle() == QString::fromStdString(details.display_name));
    CHECK(dialog.windowModality() == Qt::WindowModal);
    CHECK(dialog.minimumWidth() == 460);
    CHECK(dialog.maximumWidth() == 520);
    CHECK(dialog.minimumHeight() == 420);
    CHECK(dialog.nameLabel()->text() == QString::fromStdString(details.display_name));
    CHECK(dialog.descriptionLabel()->text() == QString::fromStdString(details.description));
    CHECK(dialog.bestForLabel()->text() == QString::fromStdString(details.best_for));
    CHECK(dialog.sourceNote()->text() == QString::fromStdString(details.source_note));

    CHECK(dialog.factLabel("Origin")->text() == QString::fromStdString(details.origin_name));
    CHECK(dialog.factLabel("Source")->text() == QString::fromStdString(details.source_name));
    CHECK(dialog.factLabel("Maintainer")->text() == QString::fromStdString(details.maintainer));
    CHECK(dialog.factLabel("Requires")->text() == QString::fromStdString(details.requires_text));
    CHECK(dialog.factLabel("Payload")->text() == QString::fromStdString(details.payload));
    CHECK(dialog.factLabel("Download size")->text() == "117 MB");
    CHECK(dialog.factLabel("Install size")->text() == "410 MB");
    CHECK(dialog.factLabel("Local format")->text() == QString::fromStdString(details.local_format));
    CHECK(dialog.factLabel("License")->text() == QString::fromStdString(details.license));
    CHECK(dialog.factLabel("Nothing") == nullptr);
    CHECK(dialog.factLabel("License")->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));

    QStringList bullets;
    for (const std::string& item : details.limitations) {
        bullets.append(QString::fromUtf8("\u2022 ") + QString::fromStdString(item));
    }
    CHECK(dialog.tradeoffsLabel()->text() == bullets.join('\n'));
}

TEST_CASE("the component popup writes a dash for a size it does not know", "[widget]") {
    fvtest::AppTheme theme;
    fv::models::ComponentInfo info = cudaInfo();
    info.download_bytes = 0;
    info.install_bytes = 0;
    ComponentDetailsDialog dialog("asr-nvidia-cuda", info);
    CHECK(dialog.factLabel("Download size")->text() == QString::fromUtf8("\u2014"));
    CHECK(dialog.factLabel("Install size")->text() == QString::fromUtf8("\u2014"));
}

TEST_CASE("the component popup's link buttons open the source and the origin", "[widget]") {
    fvtest::AppTheme theme;
    // The CPU and GPU runtimes publish one upstream project for both, so the origin button is
    // hidden; a component whose two URLs differ shows both.
    for (const auto& details : fv::models::component_catalog()) {
        CAPTURE(details.component_id);
        ComponentDetailsDialog dialog(details.component_id, cudaInfo());
        std::vector<QUrl> opened;
        dialog.setUrlOpener([&opened](const QUrl& url) { opened.push_back(url); });

        CHECK(dialog.sourceButton()->text() == QString::fromStdString(details.source_label));
        CHECK(dialog.sourceButton()->toolTip() == QString::fromStdString(details.source_url));
        CHECK(dialog.originButton()->isHidden() == (details.origin_url == details.source_url));

        dialog.sourceButton()->click();
        dialog.originButton()->click();
        REQUIRE_FALSE(opened.empty());
        CHECK(opened[0] == QUrl(QString::fromStdString(details.source_url)));
        REQUIRE(opened.size() == 2);
        CHECK(opened[1] == QUrl(QString::fromStdString(details.origin_url)));
    }
}

TEST_CASE("the component popup closes with Close", "[widget]") {
    fvtest::AppTheme theme;
    ComponentDetailsDialog dialog("asr-nvidia-cuda", cudaInfo());
    fvtest::buttonText(dialog, "Close")->click();
    CHECK(dialog.result() == QDialog::Accepted);
}

TEST_CASE("a component without a profile is refused", "[widget]") {
    fvtest::AppTheme theme;
    CHECK_THROWS_AS(ComponentDetailsDialog("no-such-component", cudaInfo()), std::out_of_range);
}

TEST_CASE("the Downloads feature has its id and needs no other feature", "[unit]") {
    fv::DownloadsFeature feature;
    CHECK(feature.id() == "downloads");
    CHECK(feature.required_features().empty());
}
