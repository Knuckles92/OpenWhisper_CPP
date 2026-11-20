#pragma once

#include <string>
#include <string_view>

#include "credentials/store.h"

// Webhook actions' signing secrets in the credential store (services/actions/runtime.py
// ensure_action_secret and delete_action_secret), under action_secret_name(id). Never logged.
namespace fv::automation {

// The credential store could not read, save or delete a secret; the message names no secret.
class SecretUnavailable : public CredentialStoreError {
public:
    using CredentialStoreError::CredentialStoreError;
};

// A webhook action's signing secret, created (signing::new_secret) and saved on first use.
// Blocking, never on the GUI thread: the store may ask the desktop to unlock it. One caller at a
// time, so two first uses agree on one secret. Throws SecretUnavailable when it cannot be saved.
std::string ensure_action_secret(CredentialStore& store, std::string_view action_id);

// Forgets a deleted webhook action's secret; blocking. Throws SecretUnavailable when the store
// refuses.
void delete_action_secret(CredentialStore& store, std::string_view action_id);

} // namespace fv::automation
