// KeyPresence: whether a key is saved, for the guided setup's provider row, looked up on a worker
// (the credential store never runs on the GUI thread) and kept for providerSummary. Both executors
// are manual, so the test decides when the lookup runs and when its answer lands.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <QSignalSpy>

#include "base/testing/manual_executor.h"
#include "ui_setup/cleanup_provider_row.h"

namespace {

// The keys a lookup finds, and how many lookups ran.
struct Keys {
    std::map<std::string, bool> saved;
    int lookups = 0;

    fv::KeyPresence::Lookup lookup() {
        return [this](const std::string& env) {
            ++lookups;
            if (env == "BROKEN_KEY") {
                throw std::runtime_error("the store is locked");
            }
            const auto found = saved.find(env);
            return found != saved.end() && found->second;
        };
    }
};

} // namespace

TEST_CASE("KeyPresence answers from its last lookup, which runs on the worker") {
    Keys keys;
    fv::ManualExecutor worker;
    fv::ManualExecutor ui;
    fv::KeyPresence presence(keys.lookup(), worker, ui);
    QSignalSpy changed(&presence, &fv::KeyPresence::changed);

    // Not saved until a lookup says so; the first question starts one, a second waits for it.
    CHECK_FALSE(presence.saved("OPENROUTER_API_KEY"));
    CHECK_FALSE(presence.saved("OPENROUTER_API_KEY"));
    CHECK(worker.queued() == 1);
    CHECK(keys.lookups == 0);
    worker.run_all();
    CHECK(keys.lookups == 1);
    ui.run_all();
    // "Not saved" is what the row already showed.
    CHECK(changed.count() == 0);
    CHECK_FALSE(presence.saved("OPENROUTER_API_KEY"));
    CHECK(worker.queued() == 0);

    // A key saved since: check() looks again, and a new answer redraws the row.
    keys.saved["OPENROUTER_API_KEY"] = true;
    presence.check("OPENROUTER_API_KEY");
    worker.run_all();
    ui.run_all();
    CHECK(changed.count() == 1);
    CHECK(presence.saved("OPENROUTER_API_KEY"));
}

TEST_CASE("KeyPresence looks again when a key changes while its lookup runs") {
    Keys keys;
    keys.saved["OPENAI_API_KEY"] = true;
    fv::ManualExecutor worker;
    fv::ManualExecutor ui;
    fv::KeyPresence presence(keys.lookup(), worker, ui);
    QSignalSpy changed(&presence, &fv::KeyPresence::changed);
    presence.check("OPENAI_API_KEY");
    worker.run_all();
    ui.run_all();
    REQUIRE(presence.saved("OPENAI_API_KEY"));
    REQUIRE(changed.count() == 1);

    // The lookup read the key before it was removed, and its answer has not landed yet.
    presence.check("OPENAI_API_KEY");
    worker.run_all();
    keys.saved["OPENAI_API_KEY"] = false;
    presence.recheck();
    ui.run_all();
    // The stale answer is followed by one more lookup, which finds the key gone.
    CHECK(worker.queued() == 1);
    worker.run_all();
    ui.run_all();
    CHECK_FALSE(presence.saved("OPENAI_API_KEY"));
    CHECK(changed.count() == 2);
    CHECK(keys.lookups == 3);
}

TEST_CASE("KeyPresence rechecks a first lookup that has not landed yet") {
    Keys keys;
    fv::ManualExecutor worker;
    fv::ManualExecutor ui;
    fv::KeyPresence presence(keys.lookup(), worker, ui);
    // The controller's first lookup reads the store before the key is saved on the API keys page.
    presence.check("OPENROUTER_API_KEY");
    worker.run_all();
    keys.saved["OPENROUTER_API_KEY"] = true;
    presence.recheck();
    ui.run_all();
    worker.run_all();
    ui.run_all();
    CHECK(presence.saved("OPENROUTER_API_KEY"));
    CHECK(keys.lookups == 2);
}

TEST_CASE("KeyPresence reads a failed lookup as no key and outlives nothing") {
    Keys keys;
    fv::ManualExecutor worker;
    fv::ManualExecutor ui;
    {
        fv::KeyPresence presence(keys.lookup(), worker, ui);
        CHECK_FALSE(presence.saved("BROKEN_KEY"));
        worker.run_all();
        ui.run_all();
        CHECK_FALSE(presence.saved("BROKEN_KEY"));
        CHECK(worker.queued() == 0);

        // A lookup still queued when the row goes away.
        presence.check("GROQ_API_KEY");
    }
    worker.run_all();
    ui.run_all();
    CHECK(keys.lookups == 1);
}
