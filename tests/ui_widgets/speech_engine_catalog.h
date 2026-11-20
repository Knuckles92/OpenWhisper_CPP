#pragma once

#include <string>
#include <string_view>

#include <QString>

#include "models/speech_catalog.h"
#include "settings/keys.h"
#include "speech/dictation_language.h"
#include "speech/languages.h"
#include "ui_widgets/local_engine_controls.h"

namespace fvtest {

// What the speech catalog gives the engine fields, as the feature that builds them passes it
// (the shape ui_record's speechEngineCatalog gives Quick Record), so a test sees the real
// Parakeet models, devices and languages.
inline fv::LocalEngineCatalog speechCatalog() {
    const auto text = [](std::string_view value) {
        return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    };
    fv::LocalEngineCatalog catalog;
    catalog.knows = [](const QString& backend) {
        return fv::models::backend(backend.toStdString()) != nullptr;
    };
    catalog.models = [text](const QString& backend) {
        std::vector<fv::LocalEngineCatalog::Model> found;
        const std::string wanted = backend.toStdString();
        for (const fv::models::SpeechModel& model : fv::models::models()) {
            if (model.backend == wanted) {
                found.push_back({text(model.key), text(model.label)});
            }
        }
        return found;
    };
    catalog.selectedModel = [text](const fv::Json& settings, const QString& backend) {
        return text(fv::models::selected_model(settings, backend.toStdString()));
    };
    catalog.selectedDevice = [text](const fv::Json& settings, const QString& backend) {
        return text(fv::models::selected_device(settings, backend.toStdString()));
    };
    catalog.languages = [text](const QString& backend) {
        std::vector<std::pair<QString, QString>> found;
        const std::string key = backend.toStdString();
        for (const std::string_view code : fv::speech::language_choices(std::string_view(key))) {
            QString label = text(code);
            for (const auto& [known, name] : fv::speech::language_labels()) {
                if (known == code) {
                    label = text(name);
                }
            }
            found.emplace_back(text(code), label);
        }
        return found;
    };
    catalog.selectedLanguage = [text](const QString& backend, const QString& stored) {
        return text(fv::speech::selected_language(backend.toStdString(), stored.toStdString()));
    };
    catalog.dictationDecidesLanguage = [](const fv::Json& settings, const QString& backend) {
        fv::Json selected = settings.is_object() ? settings : fv::Json::object();
        selected[std::string(fv::keys::selected_model.key())] = backend.toStdString();
        return !fv::speech::job_language(selected).empty() &&
               fv::speech::single_language_reason(selected).empty();
    };
    return catalog;
}

} // namespace fvtest
