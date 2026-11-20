#include "ui_controller/process_judge.h"

#include <memory>
#include <utility>

#include "appkit/app_context.h"
#include "base/env.h"
#include "base/paths.h"
#include "credentials/dotenv.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "http/client.h"
#include "llm/judge.h"

namespace fv {
namespace {

// Everything the process's judges read, owned together and released with the last holder.
struct JudgeParts {
    std::shared_ptr<CredentialStore> credentials;
    std::shared_ptr<http::Client> http;
    std::unique_ptr<DotEnv> dotenv;
    std::unique_ptr<llm::JudgeEnvironment> environment;
    std::unique_ptr<llm::JudgeProvider> provider;
};

// The provider the context hands out: forwards to the one built over the parts it owns.
class OwningJudgeProvider final : public llm::JudgeProvider {
public:
    explicit OwningJudgeProvider(std::shared_ptr<JudgeParts> parts) : parts_(std::move(parts)) {}

    std::shared_ptr<llm::Judge> judge(const Json& settings) override {
        return parts_->provider->judge(settings);
    }
    llm::JudgeRoute route(const Json& settings) override {
        return parts_->provider->route(settings);
    }
    bool key_present(const Json& settings) override {
        return parts_->provider->key_present(settings);
    }

private:
    std::shared_ptr<JudgeParts> parts_;
};

} // namespace

std::shared_ptr<llm::JudgeProvider> provideProcessJudge(AppContext& context) {
    if (auto provided = context.find_shared<llm::JudgeProvider>()) {
        return provided;
    }
    auto parts = std::make_shared<JudgeParts>();
    // Its own store and client: a judge may still run after the app's are closed at quit.
    parts->credentials =
        std::make_shared<CredentialStore>(make_platform_backend(), context.identity());
    parts->http = http::make_curl_client();
    parts->dotenv = std::make_unique<DotEnv>(context.paths().env_file(), system_env());
    parts->environment = std::make_unique<llm::JudgeEnvironment>(parts->http, *parts->credentials,
                                                                 system_env(), *parts->dotenv);
    parts->provider = llm::make_judge_provider(*parts->environment);
    auto provider = std::make_shared<OwningJudgeProvider>(parts);
    context.provide<llm::JudgeProvider>(provider);
    if (context.find<llm::JudgeEnvironment>() == nullptr) {
        // JudgeKeys::over(AppContext&) verifies keys over it instead of building its own.
        context.provide<llm::JudgeEnvironment>(
            std::shared_ptr<llm::JudgeEnvironment>(parts, parts->environment.get()));
    }
    return provider;
}

} // namespace fv
