// The text-model picker and the endpoint dialog against the references OpenWhisper rendered for
// them (tools/parity/builders/a13.py, tests/reference/<widget>/<variant>/<theme>-windows11-dpr1-
// s<scale>.{png,json}): the widget tree first, then the pixels, with the application styled as
// OpenWhisper styles it.
//
// The picker is given the credential line a host that found OpenRouter's key would hand it, as
// the builder replaces credential_status with the same answer.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>

#include <QCoreApplication>
#include <QDialog>
#include <QWidget>

#include "app_theme.h"
#include "base/json.h"
#include "llm/profiles.h"
#include "screenshot/screenshot.h"
#include "ui_widgets/text_endpoint_dialog.h"
#include "ui_widgets/text_model_picker.h"

namespace {

using fv::Json;
namespace llm = fv::llm;

void check(const std::string& widget, const std::string& theme, int scale,
           const std::function<std::unique_ptr<QWidget>()>& build) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    const std::unique_ptr<QWidget> shown = build();
    if (auto* dialog = qobject_cast<QDialog*>(shown.get())) {
        // As the builder does: shown once and hidden again to learn the size it opens at (and
        // to leave no widget holding the focus, which the reference's tree records).
        dialog->show();
        QCoreApplication::processEvents();
        dialog->hide();
    }
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    fv::screenshot::expect_matches(*shown, widget, variant, {});
}

#define REFERENCE_CASE(name, ...)                                                                  \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, "dark", 100, __VA_ARGS__);                                                     \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, "light", 100, __VA_ARGS__);                                                    \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " dark 130", "[screenshot]") {                          \
        check(name, "dark", 130, __VA_ARGS__);                                                     \
    }

std::unique_ptr<fv::TextModelPicker> newPicker() {
    auto picker = std::make_unique<fv::TextModelPicker>();
    picker->setProfiles(llm::list_profiles(Json::object()));
    return picker;
}

} // namespace

REFERENCE_CASE("text_model_picker/openrouter", [] {
    auto picker = newPicker();
    picker->setProvider(QStringLiteral("openrouter"));
    picker->setCredentialStatus(QStringLiteral("OpenRouter API key found"), true,
                                QStringLiteral("https://openrouter.ai/api/v1"));
    picker->setSort(QStringLiteral("most-popular"));
    picker->setActiveSelection(QStringLiteral("openrouter"), QStringLiteral("openrouter/free"));
    picker->setModels(
        {QStringLiteral("openrouter/free"), QStringLiteral("anthropic/claude-sonnet-4.5")});
    picker->setStatus(QStringLiteral("2 models available"));
    return picker;
})

REFERENCE_CASE("text_model_picker/local-running", [] {
    auto picker = newPicker();
    picker->setRunningServers(std::map<QString, QString>{
        {QStringLiteral("http://localhost:11434/v1"), QStringLiteral("ollama")}});
    picker->setProvider(QStringLiteral("ollama"), QStringLiteral("llama3.2"));
    picker->setModels({QStringLiteral("llama3.2")});
    picker->setStatus(QStringLiteral("1 models available"));
    return picker;
})

REFERENCE_CASE("text_endpoint_dialog/add",
               [] { return std::make_unique<fv::TextEndpointDialog>(); })

REFERENCE_CASE("text_endpoint_dialog/local-preset", [] {
    return std::make_unique<fv::TextEndpointDialog>(
        llm::get_profile(std::string_view("lm_studio"), Json::object()));
})
