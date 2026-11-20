#include "ui_widgets/speech_server_dialog.h"

#include <exception>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/identity.h"
#include "base/log.h"
#include "settings/store.h"
#include "speech/api_models.h"
#include "ui_theme/app_icon.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.speech_server_dialog"};

} // namespace

SpeechServerDialog::SpeechServerDialog(SettingsStore& settings,
                                       const speech::SpeechEndpoint& endpoint, QWidget* parent)
    : QDialog(parent), settings_(settings), endpoint_(endpoint) {
    const QString name = qt::toQString(endpoint.name);
    setWindowTitle(QStringLiteral("%1 server").arg(name));
    setWindowIcon(ui::appIcon());
    setObjectName(QStringLiteral("speechServerDialog"));
    setModal(true);
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("%1 server").arg(name));
    title->setObjectName(QStringLiteral("headerLabel"));
    layout->addWidget(title);
    auto* intro = new QLabel(
        QStringLiteral("%1 %2 sends recordings to its OpenAI-compatible /audio/transcriptions "
                       "route. A server on this computer keeps audio here. Name the variable "
                       "that holds its API key, then enter the key under Settings → App → API "
                       "keys, or leave it blank for a server that does not authenticate.")
            .arg(qt::toQString(endpoint.description), qt::toQString(identity().product)));
    intro->setObjectName(QStringLiteral("infoLabel"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    urlEdit_ = new QLineEdit(qt::toQString(endpoint.base_url.value_or("")));
    urlEdit_->setObjectName(QStringLiteral("speechServerUrlEdit"));
    urlEdit_->setPlaceholderText(QStringLiteral("http://127.0.0.1:8000/v1"));
    modelEdit_ = new QLineEdit(qt::toQString(speech::endpoint_model(
        endpoint, *settings.snapshot(), speech::local_today(system_clock()))));
    modelEdit_->setObjectName(QStringLiteral("speechServerModelEdit"));
    envEdit_ = new QLineEdit(qt::toQString(endpoint.api_key_env));
    envEdit_->setObjectName(QStringLiteral("speechServerEnvEdit"));
    envEdit_->setPlaceholderText(QStringLiteral("Optional, e.g. SPEACHES_API_KEY"));
    form->addRow(QStringLiteral("Base URL"), urlEdit_);
    form->addRow(QStringLiteral("Model"), modelEdit_);
    form->addRow(QStringLiteral("API key variable"), envEdit_);
    layout->addLayout(form);

    errorLabel_ = new QLabel;
    errorLabel_->setObjectName(QStringLiteral("speechServerError"));
    errorLabel_->setWordWrap(true);
    layout->addWidget(errorLabel_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* cancel = new Button(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    saveButton_ = new PrimaryButton(QStringLiteral("Save server"));
    ui::setStyleProperty(saveButton_, ui::prop::buttonId, QStringLiteral("speechServerSaveButton"));
    connect(saveButton_, &QPushButton::clicked, this, &SpeechServerDialog::save);
    buttons->addWidget(cancel);
    buttons->addWidget(saveButton_);
    layout->addLayout(buttons);
}

void SpeechServerDialog::save() {
    const std::string baseUrl = urlEdit_->text().toStdString();
    const std::string apiKeyEnv = envEdit_->text().toStdString();
    const std::string model = modelEdit_->text().toStdString();
    const std::string id = endpoint_.id;
    try {
        settings_.mutate(
            [&](Json& all) { speech::save_server(all, id, baseUrl, apiKeyEnv, model); });
    } catch (const std::exception& error) {
        // std::invalid_argument carries the sentence to show; anything else is a failed write.
        if (dynamic_cast<const std::invalid_argument*>(&error) == nullptr) {
            kLog.error_with(error, "Couldn't save the speech server");
        }
        errorLabel_->setText(qt::toQString(error.what()));
        return;
    }
    const Json now = *settings_.snapshot();
    const speech::SpeechEndpoint stored = speech::configured_endpoint(id, now);
    saved_ = Saved{stored.base_url.value_or(""), stored.api_key_env,
                   speech::endpoint_model(stored, now, speech::local_today(system_clock()))};
    accept();
}

} // namespace fv
