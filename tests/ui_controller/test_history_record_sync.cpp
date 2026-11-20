// History reaches the record sync the remote engine provides (history_manager._record_sync):
// saves, edits, deletes and clears are told to it, as OpenWhisper's HistoryManager tells
// record_sync. What the record sync does with them is its own (tests/remote_records).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "controller_support.h"
#include "history/manager.h"
#include "remote/testing/fake_record_sync.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;

TEST_CASE("History tells the provided record sync what changed", "[A17-49][widget]") {
    auto records = std::make_shared<fv::remote::testing::FakeRecordSync>();
    RigOptions options;
    options.beforeFeatures = [records](fv::AppContext& context) {
        context.provide<fv::remote::RecordSyncControl>(records);
    };
    ControllerRig rig(options);
    auto* history = rig.context().find<fv::HistoryManager>();
    REQUIRE(history != nullptr);

    const auto id = fvtest::offGui([history] {
        fv::AddEntry fields;
        fields.text = "Hello.";
        fields.raw_text = "hello um";
        fields.model = "base";
        const auto saved = history->add_entry(fields).value();
        (void)history->use_version(saved.id, fv::ORIGINAL_VERSION);
        history->delete_entry(saved.id);
        history->clear_history();
        return saved.id;
    });
    const std::vector<std::string> expected{
        "record_saved(dictation," + id + ")", "record_edited(dictation," + id + ")",
        "record_deleted(dictation," + id + ")", "cleared(dictation)"};
    CHECK(records->calls() == expected);
}

TEST_CASE("without a record sync History keeps everything here", "[A17-49][widget]") {
    ControllerRig rig;
    auto* history = rig.context().find<fv::HistoryManager>();
    REQUIRE(history != nullptr);
    CHECK(rig.context().find<fv::remote::RecordSyncControl>() == nullptr);
    CHECK(fvtest::offGui([history] {
        fv::AddEntry fields;
        fields.text = "Hello.";
        fields.model = "base";
        return history->add_entry(fields).has_value();
    }));
}
