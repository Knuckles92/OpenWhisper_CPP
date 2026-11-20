#pragma once

// The Settings window with the App pages and the Fast judgments page over fakes: an in-memory
// credential store with an environment of its own, a real worker pool, and a judge whose key
// check records what it was asked and answers as scripted (OpenWhisper's
// tests/test_typesafe_settings_messaging.py patches typesafe_verify_key the same way). Every
// other destination is a stand-in page.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app_pages_support.h"
#include "llm/judge.h"
#include "llm/typesafe.h"
#include "ui_settings/api_keys_section.h"
#include "ui_settings/fast_judgments_page.h"
#include "ui_settings/typesafe_credential.h"
#include "ui_widgets/nav_rail.h"

namespace fvtest {

inline const std::string kTypeSafeEnv = "TYPESAFE_API_KEY";
inline const std::string kOpenRouterEnv = "OPENROUTER_API_KEY";
inline const std::string kTypeSafeKey = "ts-live-0123456789abcdefghij";
inline const std::string kOpenRouterKey = "sk-or-v1-0123456789abcdefghij";

// What the judge's key check was asked: the route it was asked over, its model and the key.
struct Verified {
    std::string route;
    std::string model;
    std::string key;
};

class FastJudgmentsRig {
public:
    explicit FastJudgmentsRig(const fv::Json& settings = fv::Json::object())
        : rig_(settings, [this](AppPagesRig& rig) { install(rig); }) {}

    AppPagesRig& rig() { return rig_; }
    fv::SettingsDialog& dialog() { return rig_.dialog(); }

    // Stores a key in the credential store (it blocks, so off the GUI thread).
    void setKey(const std::string& name, const std::string& value) {
        offGui([this, &name, &value] { rig_.store().set(name, value); });
    }

    // The Fast judgments page, built, with the keys it shows read.
    fv::FastJudgmentsPage& page() {
        auto& built = rig_.page<fv::FastJudgmentsPage>(dest::meeting_fast);
        settle(built);
        return built;
    }
    void settle(fv::FastJudgmentsPage& built) {
        REQUIRE(pumpUntil([&built] { return built.stateCurrent(); }));
    }
    // The page's rail value once the keys it counts are read.
    QString rail() {
        dialog().refreshRailValues();
        return dialog().rail()->value(qs(dest::meeting_fast));
    }
    // The API keys page, with its credentials read.
    fv::ApiKeysSection& keysPage() {
        auto& keys = rig_.page<fv::ApiKeysSection>(dest::api_keys);
        REQUIRE(pumpUntil([&keys] { return keys.stateCurrent(); }));
        return keys;
    }
    // A save, removal or test on the API keys page finished.
    void settleKeys(fv::ApiKeysSection& keys) {
        REQUIRE(pumpUntil(
            [&keys] { return !keys.storing() && !keys.testing() && keys.stateCurrent(); }));
    }

    std::vector<Verified> verifiedNow() {
        std::lock_guard lock(mutex_);
        return verified_;
    }
    // What a key check answers from now on.
    void answer(bool ok, const std::string& message) {
        std::lock_guard lock(mutex_);
        answer_ = {ok, message};
    }

private:
    void install(AppPagesRig& rig) {
        // The API keys page announces a saved key on the signals AppPages owns.
        rig.context().provide<fv::SettingsPagesSignals>(rig.pages().sharedSignals());
        keys_ = std::make_shared<fv::JudgeKeys>(
            fv::llm::KeySources{rig.store(), rig.env(), rig.dotenv()},
            [this](const fv::llm::JudgeRoute& route, const std::string& key) {
                std::lock_guard lock(mutex_);
                verified_.push_back({route.id, route.model, key});
                return answer_;
            });
        fv::FastJudgmentsOptions options;
        options.keys = keys_;
        options.worker = &rig.pool();
        fv::AppPages* pages = &rig.pages();
        options.dialog = [pages] {
            return pages->hooks().dialog();
        };
        fv::registerFastJudgmentsPage(rig.context(), std::move(options));
    }

    std::mutex mutex_;
    std::vector<Verified> verified_;
    fv::llm::VerifyResult answer_{true, "api.typesafe.ai accepted the key."};
    std::shared_ptr<fv::JudgeKeys> keys_;
    AppPagesRig rig_;
};

} // namespace fvtest
