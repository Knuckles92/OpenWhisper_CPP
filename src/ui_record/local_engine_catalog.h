#pragma once

#include "ui_widgets/local_engine_controls.h"

namespace fv {

// The speech catalog as the local engine fields read it: the Parakeet models (models/
// speech_catalog.h) and the languages the runtime takes (speech/languages.h).
LocalEngineCatalog speechEngineCatalog();

} // namespace fv
