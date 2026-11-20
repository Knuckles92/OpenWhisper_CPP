// "Paste original" after this computer's dictations moved to the paired host
// (tests/test_personalize_fix_f1_paste_original_host.py): the last dictation is the newest one
// made here, even once its row went to the host. FioriVoce moves records when the user asks
// (Move existing records, a17-retention), never a second after a save, so OpenWhisper's
// "leftover from before the switch" cannot arise: the newest row kept here is this computer's
// newest dictation.
#include <catch2/catch_test_macros.hpp>

#include "sync_rig.h"

using namespace fv;
using records_rig::Paired;

namespace {

// Move existing records, as the Remote engine page's button sends them.
void move_everything(Paired& rig) {
    rig.sync->set_location("host");
    REQUIRE(rig.sync->send_existing() > 0);
    rig.run();
    REQUIRE(rig.sync->status().pending == 0);
}

} // namespace

TEST_CASE("test_the_moved_dictation_is_pasted_not_an_old_local_one",
          "[test_personalize_fix_f1_paste_original_host]") {
    Paired rig;
    const auto old = rig.dictate("Old meeting notes.", "old meeting notes um");
    const auto made = rig.dictate("Send the invoice today.", "send the invoice today um");
    move_everything(rig);
    CHECK_FALSE(rig.history->get_entry(made.id));
    REQUIRE(rig.on_host(made.id));

    const auto last = rig.history->last_dictation();
    REQUIRE(last);
    CHECK(last->id == made.id);
    CHECK(last->raw_text == "send the invoice today um");
    CHECK(last->stored_on == "the host");
    CHECK(rig.on_host(made.id)->text == "Send the invoice today.");
    CHECK(rig.on_host(old.id)->text == "Old meeting notes.");
}

TEST_CASE("test_host_only_storage_still_pastes_the_newest_original",
          "[test_personalize_fix_f1_paste_original_host]") {
    Paired rig;
    rig.dictate("Send the invoice today.", "send the invoice today um");
    move_everything(rig);
    const auto last = rig.history->last_dictation();
    REQUIRE(last);
    CHECK(last->raw_text == "send the invoice today um");
}

TEST_CASE("test_clearing_history_forgets_the_moved_dictation",
          "[test_personalize_fix_f1_paste_original_host]") {
    Paired rig;
    rig.dictate("Send the invoice today.", "send the invoice today um");
    move_everything(rig);
    REQUIRE(rig.history->last_dictation());
    rig.history->clear_history();
    CHECK_FALSE(rig.history->last_dictation());
}

TEST_CASE("test_deleting_the_moved_dictation_on_the_host_forgets_it",
          "[test_personalize_fix_f1_paste_original_host]") {
    // History's delete of a host-kept entry: the host deletes it, then History forgets it
    // (HistorySidebar::deleteRemoteEntry; its card half is in tests/ui_history).
    Paired rig;
    const auto made = rig.dictate("Send the invoice today.", "send the invoice today um");
    move_everything(rig);
    CHECK(rig.sync->delete_remote("dictation", made.id));
    rig.history->forget_dictation(made.id);
    CHECK_FALSE(rig.on_host(made.id));
    CHECK_FALSE(rig.history->last_dictation());
}

TEST_CASE("test_kept_here_after_a_restart_the_newest_local_dictation_still_counts",
          "[test_personalize_fix_f1_paste_original_host]") {
    Paired rig;
    rig.dictate("Hello, world.", "um hello world");
    // A restart: a new History over the same database remembers nothing of this session.
    HistoryManager restarted(rig.client_db, rig.recordings(), RecordingRetention{},
                             HistoryServices{.record_sync = rig.sync.get(), .clock = &rig.clock});
    const auto last = restarted.last_dictation();
    REQUIRE(last);
    CHECK(last->raw_text == "um hello world");
}
