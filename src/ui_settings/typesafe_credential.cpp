#include "ui_settings/typesafe_credential.h"

#include <exception>
#include <filesystem>
#include <mutex>
#include <utility>

#include "appkit/app_context.h"
#include "base/env.h"
#include "base/log.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "http/client.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_fast_judgments"};

// What JudgeKeys::over(AppContext&) keeps: the .env file, and the environment built on first use
// when nothing provided one.
struct ContextKeys {
    ContextKeys(AppContext& context, std::filesystem::path env_file)
        : context(context), dotenv(std::move(env_file)) {}

    AppContext& context;
    DotEnv dotenv;
    std::mutex mutex;
    std::unique_ptr<llm::JudgeEnvironment> own;
};

} // namespace

namespace typesafe_credential {

appcore::CredentialOwner owner() {
    return {std::string(llm::kTypeSafeCredentialEnv), "TypeSafe",
            std::string(appcore::kUseDecisions)};
}

bool is_typesafe(std::string_view env_name) {
    return env_name == llm::kTypeSafeCredentialEnv;
}

} // namespace typesafe_credential

JudgeKeys::JudgeKeys(llm::KeySources keys, Verify verify, std::shared_ptr<void> keep)
    : keys_(keys), verify_(std::move(verify)), keep_(std::move(keep)) {}

std::shared_ptr<JudgeKeys> JudgeKeys::over(llm::JudgeEnvironment& environment) {
    llm::JudgeEnvironment* target = &environment;
    return std::make_shared<JudgeKeys>(
        environment.keys(), [target](const llm::JudgeRoute& route, const std::string& key) {
            return llm::verify_key(route, key, *target);
        });
}

std::shared_ptr<JudgeKeys> JudgeKeys::over(AppContext& context) {
    std::filesystem::path env_file;
    try {
        env_file = context.paths().env_file();
    } catch (const ServiceUnavailable&) {
        // No data folder: a missing .env reads as empty.
    }
    auto state = std::make_shared<ContextKeys>(context, std::move(env_file));
    llm::KeySources keys{context.credentials(), system_env(), state->dotenv};
    auto verify = [state, keys](const llm::JudgeRoute& route, const std::string& key) {
        llm::JudgeEnvironment* environment = state->context.find<llm::JudgeEnvironment>();
        if (environment == nullptr) {
            const std::lock_guard lock(state->mutex);
            if (state->own == nullptr) {
                // The context owns its client; this environment only borrows it.
                std::shared_ptr<http::Client> borrowed(&state->context.http(),
                                                       [](http::Client*) {});
                state->own = std::make_unique<llm::JudgeEnvironment>(
                    std::move(borrowed), keys.store, keys.env, keys.dotenv);
            }
            environment = state->own.get();
        }
        return llm::verify_key(route, key, *environment);
    };
    return std::make_shared<JudgeKeys>(keys, std::move(verify), state);
}

std::optional<std::string> JudgeKeys::key(const llm::JudgeRoute& route) {
    try {
        return llm::route_key(route, keys_);
    } catch (const std::exception& error) {
        // The type only: what() may name a key.
        kLog.warning("Couldn't look up the {} key ({})", route.name,
                     type_display_name(typeid(error)));
        return std::nullopt;
    }
}

bool JudgeKeys::present(const llm::JudgeRoute& route) {
    const std::optional<std::string> found = key(route);
    return found.has_value() && !found->empty();
}

bool JudgeKeys::presentOutsideStore(const llm::JudgeRoute& route) {
    if (route.credential_env.empty()) {
        return true;
    }
    try {
        const std::optional<std::string> fromEnv = keys_.env.get(route.credential_env);
        if (fromEnv.has_value()) {
            // Set but empty: loading .env never overrides a variable that is set.
            return !fromEnv->empty();
        }
        const std::optional<std::string> fromFile = keys_.dotenv.get(route.credential_env);
        return fromFile.has_value() && !fromFile->empty();
    } catch (const std::exception& error) {
        kLog.warning("Couldn't look up the {} key ({})", route.name,
                     type_display_name(typeid(error)));
        return false;
    }
}

llm::VerifyResult JudgeKeys::verify(const llm::JudgeRoute& route, const std::string& key) {
    try {
        if (!verify_) {
            throw std::logic_error("No way to verify a key.");
        }
        return verify_(route, key);
    } catch (const std::exception&) {
        // Never the message: a provider's error can echo the key back.
        return {false, "Verification failed."};
    }
}

std::optional<llm::VerifyResult> JudgeKeys::verifyRoute(const llm::JudgeRoute& route) {
    const std::optional<std::string> found = key(route);
    if (!found || found->empty()) {
        return std::nullopt;
    }
    return verify(route, *found);
}

} // namespace fv
