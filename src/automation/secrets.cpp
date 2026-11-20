#include "automation/secrets.h"

#include <mutex>
#include <stdexcept>

#include "automation/action_model.h"
#include "automation/signing.h"

namespace fv::automation {

namespace {

std::mutex& secret_mutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

std::string ensure_action_secret(CredentialStore& store, std::string_view action_id) {
    const std::string name = action_secret_name(action_id);
    std::lock_guard lock(secret_mutex());
    try {
        std::optional<std::string> secret = store.get(name);
        if (!secret || secret->empty()) {
            secret = signing::new_secret();
            store.set(name, *secret);
        }
        return *secret;
    } catch (const SecretUnavailable&) {
        throw;
    } catch (const CredentialStoreError& error) {
        throw SecretUnavailable(error.what());
    } catch (const std::invalid_argument& error) {
        throw SecretUnavailable(error.what());
    }
}

void delete_action_secret(CredentialStore& store, std::string_view action_id) {
    const std::string name = action_secret_name(action_id);
    std::lock_guard lock(secret_mutex());
    try {
        store.remove(name);
    } catch (const CredentialStoreError& error) {
        throw SecretUnavailable(error.what());
    } catch (const std::invalid_argument& error) {
        throw SecretUnavailable(error.what());
    }
}

} // namespace fv::automation
