#pragma once

// The controller's feature graph with the Personalize feature linked, a credential store with no
// keys, and an OpenAI-compatible endpoint on this computer: what the M4 wiring tests and the M4
// end-to-end test build on. The endpoint is a custom one with no key variable, so nothing reads a
// key; a test that needs AI cleanup to be unready names a variable no environment holds.

#include <memory>
#include <string>
#include <vector>

#include "controller_support.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "settings/hotkeys.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"

namespace fvtest {

// A credential store over memory, under the product's service name, and a network client that
// fails every request (nothing is scripted), for a rig that reaches no server.
struct MemoryKeys {
    std::shared_ptr<fv::MemoryCredentialBackend> backend =
        std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store{backend, fv::identity()};
    fv::http::FakeHttpClient offline;
};

// A custom text-model endpoint at `baseUrl` that needs no key (or the variable `keyEnv`), chosen
// for AI cleanup with `model`.
inline fv::Json customEndpoint(const std::string& baseUrl, const std::string& model,
                               const std::string& keyEnv = {}) {
    return fv::Json{{"transcript_cleanup_provider", "custom_e2e00001"},
                    {"transcript_cleanup_model", model},
                    {"text_llm_profiles", fv::Json::array({fv::Json{{"id", "custom_e2e00001"},
                                                                    {"name", "Test endpoint"},
                                                                    {"kind", "custom"},
                                                                    {"base_url", baseUrl},
                                                                    {"api_key_env", keyEnv}}})}};
}

// RigOptions for the M4 graph: Personalize linked, the memory key store, and `http` (null: the
// offline client, which no request gets through).
inline RigOptions personalizeOptions(MemoryKeys& keys, fv::http::Client* http,
                                     fv::Json settings = fv::Json::object()) {
    RigOptions options;
    options.personalize = true;
    options.credentials = &keys.store;
    options.http = http != nullptr ? http : &keys.offline;
    for (const auto& [key, value] : settings.items()) {
        options.settings[key] = value;
    }
    return options;
}

// The app's Settings window, built.
inline fv::SettingsDialog& settingsDialog(ControllerRig& rig) {
    return rig.context().service<fv::SettingsWindow>().ensureDialog();
}

} // namespace fvtest
