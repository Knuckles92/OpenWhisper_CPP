#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/identity.h"
#include "base/json.h"

namespace fv {
class CredentialStore;
}

// The API keys and secrets an OpenWhisper import brings over (architecture 7.5 step 4): every
// name OpenWhisper's code reads a saved key under, read from OpenWhisper's credential service
// and saved under this app's, with OpenWhisper's environment prefix replaced by this app's.
// Values are never logged. Blocking: never on the GUI thread (CredentialStore's rule).
namespace fv::backup {

struct CopyReport {
    // The names saved in `to`, as `to` names them.
    std::vector<std::string> copied;
    // Names `from` holds nothing under.
    std::vector<std::string> skipped;
    // Names whose key could not be read or saved, as `from` names them.
    std::vector<std::string> failed;
    // `from` could not be used at all (every name is in `failed`).
    bool unreadable = false;
};

// The names OpenWhisper reads saved keys under, in this order and without repeats: the
// providers' keys (OPENAI_API_KEY, OPENROUTER_API_KEY, GROQ_API_KEY, OPENCODE_GO_API_KEY,
// OPENCODE_ZEN_API_KEY, LMSTUDIO_API_KEY, TYPESAFE_API_KEY), the remote-engine, MCP, History
// API and channel tokens, an action secret for each saved action's id
// (services/actions/model.py action_secret_name), a webhook secret for each saved webhook's id
// (services/automation/models.py webhook_secret_name, the id stripped as webhooks.py strips it),
// then each text_llm_profiles entry's api_key_env, each configurable speech server's api_key_env
// in api_transcription_servers (services/speech_endpoints.py configured_endpoint), and each local
// text server's "<id>_api_key_env" (services/text_llm.py _configured_builtin). `legacy_settings`
// is OpenWhisper's settings object before any sanitizing (the sanitizer drops api_key_env).
// `from` gives the token and secret names their prefix.
std::vector<std::string> legacy_credential_names(const Json& legacy_settings,
                                                 const AppIdentity& from = legacy_identity());

// The name a key takes here: a name with `from`'s environment prefix ("OPENWHISPER_") gets
// `to`'s ("FIORIVOCE_"); any other name is kept.
std::string imported_credential_name(std::string_view name,
                                     const AppIdentity& from = legacy_identity(),
                                     const AppIdentity& to = identity());

// copy_credentials: each of legacy_credential_names(legacy_settings, from_app) that `from` holds
// is saved in `to` under imported_credential_name(name, from_app, to_app), replacing what `to`
// held under that name; names `from` does not hold are skipped and leave `to` as it was. When
// `from` cannot be used at all, every name is failed, not skipped: nothing could be read.
CopyReport copy_credentials(CredentialStore& from, CredentialStore& to, const Json& legacy_settings,
                            const AppIdentity& from_app = legacy_identity(),
                            const AppIdentity& to_app = identity());

} // namespace fv::backup
