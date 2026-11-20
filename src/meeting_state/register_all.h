#pragma once

// The review (review_ops.h), custom-report (custom_report_ops.h) and fast (fast_ops.h) ops in
// the op vocabulary. Python's patches.py merges REVIEW_HANDLERS, FAST_HANDLERS and
// CUSTOM_REPORT_HANDLERS into its handler table at import, so every apply_ops knows them; here
// the registry adds them when it is first built, with the domains store.py's _OP_DOMAINS gives
// them. None of them is an agent op, and each checks its own actor.
// Affinity: thread-safe.
namespace fv::meeting_state {

// Makes sure the registry is built, with these ops in it; idempotent.
void register_meeting_ops();

} // namespace fv::meeting_state
