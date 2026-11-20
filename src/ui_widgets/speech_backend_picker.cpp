#include "ui_widgets/speech_backend_picker.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <QComboBox>

namespace fv {
namespace {

struct Engine {
    std::string_view id;
    std::string_view name;
};

// config.MODEL_VALUE_MAP, restricted to the engines FioriVoce has.
constexpr std::array<Engine, 3> kEngines{{
    {"api", "API"},
    {"parakeet", "Parakeet"},
    {"remote", "Remote computer"},
}};

// config.DEFAULT_BACKEND: Parakeet on every platform FioriVoce runs on.
constexpr std::string_view kDefaultId = "parakeet";

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

const Engine* find(std::string_view id) {
    const auto found = std::ranges::find(kEngines, id, &Engine::id);
    return found == kEngines.end() ? nullptr : &*found;
}

} // namespace

QString backendDisplayName(std::string_view id) {
    const Engine* engine = find(id);
    if (engine == nullptr) {
        engine = find(kDefaultId);
    }
    return toQString(engine->name);
}

void populateBackendCombo(QComboBox* combo, std::span<const std::string_view> ids) {
    std::vector<const Engine*> listed;
    for (const std::string_view id : ids) {
        if (const Engine* engine = find(id)) {
            listed.push_back(engine);
        }
    }
    // sorted(..., key=str.casefold)
    std::ranges::stable_sort(listed, [](const Engine* a, const Engine* b) {
        return toQString(a->name).toCaseFolded() < toQString(b->name).toCaseFolded();
    });
    for (const Engine* engine : listed) {
        const QString name = toQString(engine->name);
        combo->addItem(name, toQString(engine->id));
        combo->setItemData(combo->count() - 1, name, Qt::ToolTipRole);
    }
}

} // namespace fv
