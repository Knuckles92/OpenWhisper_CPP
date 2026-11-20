#include "ui_widgets/text_endpoint_dialog.h"

#include <stdexcept>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "ui_theme/app_icon.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"

namespace fv {

TextEndpointDialog::TextEndpointDialog(std::optional<llm::TextLLMProfile> profile, QWidget* parent)
    : QDialog(parent), profile_(std::move(profile)), mode_(!profile_           ? Mode::Add
                                                           : profile_->builtin ? Mode::LocalPreset
                                                                               : Mode::EditCustom) {
    const bool preset = mode_ == Mode::LocalPreset;
    setWindowTitle(preset     ? QStringLiteral("%1 server").arg(qt::toQString(profile_->name))
                   : profile_ ? QStringLiteral("Edit endpoint")
                              : QStringLiteral("Add endpoint"));
    setWindowIcon(ui::appIcon());
    setObjectName(QStringLiteral("textEndpointDialog"));
    setModal(true);
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);

    QString heading;
    QString copy;
    if (preset) {
        heading = QStringLiteral("%1 server").arg(qt::toQString(profile_->name));
        copy = QStringLiteral(u"Cleanup and meetings share this server. Change the address "
                              u"when it runs on another port or computer. If you started it "
                              u"with an API key, name the variable that holds the key, then "
                              u"enter the key under Settings → App → API keys (or export the "
                              u"variable). Leave it blank when the server does not "
                              u"authenticate.");
    } else {
        heading = profile_ ? QStringLiteral("Edit OpenAI-compatible endpoint")
                           : QStringLiteral("Add an OpenAI-compatible endpoint");
        copy = QStringLiteral(u"Servers that support OpenAI Chat Completions or Responses "
                              u"work here — LiteLLM, a second LM Studio or vLLM server, or a "
                              u"private gateway. Name the variable that holds its API key, "
                              u"then enter the key under Settings → App → API keys (or export the "
                              u"variable). Leave it blank for a local server that does not "
                              u"authenticate.");
    }
    auto* title = new QLabel(heading);
    title->setObjectName(QStringLiteral("headerLabel"));
    layout->addWidget(title);

    auto* intro = new QLabel(copy);
    intro->setObjectName(QStringLiteral("infoLabel"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    nameEdit_ = new QLineEdit;
    nameEdit_->setObjectName(QStringLiteral("textEndpointNameEdit"));
    nameEdit_->setPlaceholderText(QStringLiteral("LM Studio"));
    urlEdit_ = new QLineEdit;
    urlEdit_->setObjectName(QStringLiteral("textEndpointUrlEdit"));
    urlEdit_->setPlaceholderText(QStringLiteral("http://127.0.0.1:1234/v1"));
    envEdit_ = new QLineEdit;
    envEdit_->setObjectName(QStringLiteral("textEndpointEnvEdit"));
    envEdit_->setPlaceholderText(
        QStringLiteral("Optional, e.g. ") +
        (preset ? qt::toQString(profile_->id).toUpper() + QStringLiteral("_API_KEY")
                : QStringLiteral("LMSTUDIO_API_KEY")));
    protocolCombo_ = new NoWheelComboBox;
    protocolCombo_->setObjectName(QStringLiteral("textEndpointProtocolCombo"));
    protocolCombo_->addItem(QStringLiteral("Chat Completions"), QStringLiteral("chat"));
    protocolCombo_->addItem(QStringLiteral("Responses"), QStringLiteral("responses"));
    protocolCombo_->setToolTip(QStringLiteral("Choose the API supported by your server."));
    if (profile_) {
        nameEdit_->setText(qt::toQString(profile_->name));
        urlEdit_->setText(qt::toQString(profile_->base_url.value_or(std::string())));
        envEdit_->setText(qt::toQString(profile_->api_key_env));
        protocolCombo_->setCurrentIndex(
            protocolCombo_->findData(qt::toQString(profile_->protocol)));
    }
    if (!preset) {
        form->addRow(QStringLiteral("Name"), nameEdit_);
    } else {
        unplaced_.emplace_back(nameEdit_);
    }
    form->addRow(QStringLiteral("Base URL"), urlEdit_);
    if (!preset) {
        form->addRow(QStringLiteral("API"), protocolCombo_);
    } else {
        unplaced_.emplace_back(protocolCombo_);
    }
    form->addRow(QStringLiteral("API key variable"), envEdit_);
    layout->addLayout(form);

    errorLabel_ = new QLabel;
    errorLabel_->setObjectName(QStringLiteral("textEndpointError"));
    errorLabel_->setWordWrap(true);
    layout->addWidget(errorLabel_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch();
    cancelButton_ = neutralButton(new Button(QStringLiteral("Cancel")));
    cancelButton_->setObjectName(QStringLiteral("textEndpointCancelButton"));
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    saveButton_ = compactPrimaryButton(
        new Button(preset ? QStringLiteral("Save server") : QStringLiteral("Save endpoint")));
    saveButton_->setObjectName(QStringLiteral("textEndpointSaveButton"));
    ui::setStyleProperty(saveButton_, ui::prop::buttonId, QStringLiteral("textEndpointSaveButton"));
    connect(saveButton_, &QPushButton::clicked, this, &TextEndpointDialog::save);
    buttons->addWidget(cancelButton_);
    buttons->addWidget(saveButton_);
    layout->addLayout(buttons);
}

TextEndpointDialog::~TextEndpointDialog() = default;

void TextEndpointDialog::save() {
    try {
        Payload fields;
        fields.name = llm::validate_profile_name(nameEdit_->text().toStdString());
        fields.baseUrl = mode_ == Mode::LocalPreset
                             ? llm::normalize_local_server_url(urlEdit_->text().toStdString())
                             : llm::normalize_base_url(urlEdit_->text().toStdString());
        fields.apiKeyEnv = llm::validate_api_key_env(envEdit_->text().toStdString());
        fields.protocol = llm::validate_text_protocol(
            Json(protocolCombo_->currentData().toString().toStdString()));
        if (profile_) {
            fields.id = profile_->id;
        }
        payload_ = std::move(fields);
    } catch (const std::invalid_argument& error) {
        errorLabel_->setText(qt::toQString(error.what()));
        return;
    }
    accept();
}

} // namespace fv
