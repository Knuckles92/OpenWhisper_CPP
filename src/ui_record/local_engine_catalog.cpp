#include "ui_record/local_engine_catalog.h"

#include <optional>
#include <string>
#include <string_view>

#include "models/speech_catalog.h"
#include "settings/keys.h"
#include "speech/dictation_language.h"
#include "speech/languages.h"

namespace fv {
namespace {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string toUtf8(const QString& text) {
    return text.toStdString();
}

} // namespace

LocalEngineCatalog speechEngineCatalog() {
    LocalEngineCatalog catalog;
    catalog.knows = [](const QString& backend) {
        return models::backend(toUtf8(backend)) != nullptr;
    };
    catalog.models = [](const QString& backend) {
        std::vector<LocalEngineCatalog::Model> found;
        const std::string wanted = toUtf8(backend);
        for (const models::SpeechModel& model : models::models()) {
            if (model.backend == wanted) {
                found.push_back({toQString(model.key), toQString(model.label)});
            }
        }
        return found;
    };
    catalog.selectedModel = [](const Json& settings, const QString& backend) {
        return toQString(models::selected_model(settings, toUtf8(backend)));
    };
    catalog.selectedDevice = [](const Json& settings, const QString& backend) {
        return toQString(models::selected_device(settings, toUtf8(backend)));
    };
    catalog.languages = [](const QString& backend) {
        std::vector<std::pair<QString, QString>> found;
        const std::string key = toUtf8(backend);
        for (const std::string_view code : speech::language_choices(std::string_view(key))) {
            QString label = toQString(code);
            for (const auto& [known, name] : speech::language_labels()) {
                if (known == code) {
                    label = toQString(name);
                }
            }
            found.emplace_back(toQString(code), label);
        }
        return found;
    };
    catalog.selectedLanguage = [](const QString& backend, const QString& stored) {
        return toQString(speech::selected_language(toUtf8(backend), toUtf8(stored)));
    };
    // local_engine_controls.py sync_language_field: the settings as saved, with backend selected.
    catalog.dictationDecidesLanguage = [](const Json& settings, const QString& backend) {
        Json selected = settings.is_object() ? settings : Json::object();
        selected[std::string(keys::selected_model.key())] = toUtf8(backend);
        return !speech::job_language(selected).empty() &&
               speech::single_language_reason(selected).empty();
    };
    return catalog;
}

} // namespace fv
