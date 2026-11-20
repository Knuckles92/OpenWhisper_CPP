#include "ui_setup/openwhisper_import_dialog.h"

#include <cstdint>
#include <string>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "credentials/platform_backend.h"
#include "history/format_utils.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

QString product() {
    return qt::toQString(identity().product);
}

QString legacyProduct() {
    return qt::toQString(legacy_identity().product);
}

WrappedLabel* infoLabel(const QString& text) {
    auto* label = new WrappedLabel(text);
    label->setObjectName(QStringLiteral("infoLabel"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

WrappedLabel* statusLine() {
    auto* label = new WrappedLabel(QString(), nullptr, true);
    label->setObjectName(QStringLiteral("statusLabel"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setVisible(false);
    return label;
}

// "none to bring over", one name, or the names joined by commas.
QString joined(const std::vector<QString>& names) {
    if (names.empty()) {
        return QStringLiteral("none to bring over");
    }
    QString text;
    for (const auto& name : names) {
        text += (text.isEmpty() ? QString() : QStringLiteral(", ")) + name;
    }
    return text;
}

} // namespace

OpenWhisperImportDialog::OpenWhisperImportDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("dataImportDialog"));
    setWindowTitle(QStringLiteral("Import from %1").arg(legacyProduct()));
    setModal(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    header_ = new QLabel(QStringLiteral("Bring over your %1 data").arg(legacyProduct()));
    header_->setObjectName(QStringLiteral("headerLabel"));
    root->addWidget(header_);
    root->addWidget(infoLabel(
        QStringLiteral("%1 copies your %2 settings, history and recordings, and uses the speech "
                       "runtime and model %2 already downloaded. %2's own folder is left as it "
                       "was. %1 restarts to finish.")
            .arg(product(), legacyProduct())));
    root->addWidget(infoLabel(
        QStringLiteral("As with a restored backup, automation, MCP and network sharing come over "
                       "turned off, and paired computers and the knowledge folder are left out.")));

    card_ = new Card;
    summary_ = new QVBoxLayout;
    summary_->setContentsMargins(0, 0, 0, 0);
    summary_->setSpacing(4);
    card_->cardLayout()->addLayout(summary_);
    root->addWidget(card_);

    running_ = statusLine();
    running_->setText(QStringLiteral("%1 is running. Close %1, then import.").arg(legacyProduct()));
    root->addWidget(running_);

    copyKeys_ = new QCheckBox(QStringLiteral("Copy API keys"));
    copyKeys_->setObjectName(QStringLiteral("importCopyKeysCheck"));
    copyKeys_->setChecked(false);
    root->addWidget(copyKeys_);
    root->addWidget(
        infoLabel(QStringLiteral("Saves the keys %1 kept in %2 for %3 as well.")
                      .arg(legacyProduct(), qt::toQString(platform_backend_name()), product())));

    // Shown only while the import runs: an empty bar would read as work stuck at 0 %.
    progress_ = new AnimatedProgressBar;
    progress_->setIndeterminate();
    progress_->setVisible(false);
    root->addWidget(progress_);

    status_ = statusLine();
    root->addWidget(status_);

    auto* footer = new QHBoxLayout;
    folderButton_ = new Button(QStringLiteral("Choose another folder…"));
    footer->addWidget(folderButton_);
    footer->addStretch();
    laterButton_ = new Button(QStringLiteral("Not now"));
    footer->addWidget(laterButton_);
    importButton_ = new PrimaryButton(QStringLiteral("Import and restart"));
    footer->addWidget(importButton_);
    root->addLayout(footer);

    connect(folderButton_, &QPushButton::clicked, this, [this] { Q_EMIT chooseFolderRequested(); });
    connect(laterButton_, &QPushButton::clicked, this, [this] { reject(); });
    connect(importButton_, &QPushButton::clicked, this, [this] {
        if (hasSource_ && !busy_) {
            Q_EMIT importRequested(copyKeys_->isChecked());
        }
    });

    setLooking();
}

void OpenWhisperImportDialog::clearSummary() {
    while (QLayoutItem* item = summary_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void OpenWhisperImportDialog::addLine(const QString& text) {
    summary_->addWidget(infoLabel(text));
}

std::vector<QString> OpenWhisperImportDialog::summaryLines() const {
    std::vector<QString> lines;
    for (int i = 0; i < summary_->count(); ++i) {
        if (const auto* label = qobject_cast<QLabel*>(summary_->itemAt(i)->widget())) {
            lines.push_back(label->text());
        }
    }
    return lines;
}

void OpenWhisperImportDialog::setLooking() {
    clearSummary();
    hasSource_ = false;
    addLine(QStringLiteral("Looking for %1's data…").arg(legacyProduct()));
    running_->setVisible(false);
    updateEnabled();
}

void OpenWhisperImportDialog::setFindings(const ImportFindings& findings) {
    clearSummary();
    status_->setVisible(false);
    hasSource_ = findings.source.has_value();
    if (!findings.source) {
        addLine(QStringLiteral("No %1 data was found on this computer. Choose the folder that "
                               "holds %2.")
                    .arg(legacyProduct(), qt::toQString(legacy_identity().settings_file)));
    } else {
        const backup::ImportSource& source = *findings.source;
        addLine(QStringLiteral("Folder: %1").arg(qt::toQString(path_to_utf8(source.root))));
        addLine(source.has_settings
                    ? QStringLiteral("Settings: found")
                    : QStringLiteral("Settings: none, so %1's defaults stay").arg(product()));
        addLine(source.history_entries == 1
                    ? QStringLiteral("History: 1 entry")
                    : QStringLiteral("History: %1 entries").arg(source.history_entries));
        addLine(QStringLiteral("Recordings: %1")
                    .arg(qt::toQString(
                        format_size_bytes(static_cast<std::int64_t>(source.recordings_bytes)))));
        addLine(QStringLiteral("Speech runtime: %1").arg(joined(findings.runtimes)));
        addLine(QStringLiteral("Speech model: %1").arg(joined(findings.models)));
    }
    running_->setVisible(hasSource_ && findings.running);
    updateEnabled();
}

void OpenWhisperImportDialog::setBusy(bool busy) {
    busy_ = busy;
    progress_->setVisible(busy);
    if (busy) {
        status_->setText(QStringLiteral("Bringing over your data…"));
        status_->setVisible(true);
    } else {
        status_->setVisible(false);
    }
    updateEnabled();
}

void OpenWhisperImportDialog::setError(const QString& message) {
    status_->setText(message);
    status_->setVisible(!message.isEmpty());
}

bool OpenWhisperImportDialog::copyKeys() const {
    return copyKeys_->isChecked();
}

void OpenWhisperImportDialog::reject() {
    if (!busy_) {
        QDialog::reject();
    }
}

void OpenWhisperImportDialog::updateEnabled() {
    importButton_->setEnabled(hasSource_ && !busy_);
    folderButton_->setEnabled(!busy_);
    laterButton_->setEnabled(!busy_);
    copyKeys_->setEnabled(!busy_);
}

} // namespace fv
