#pragma once

// The process-wide speaker-model state, for tests (OpenWhisper's tests monkeypatch
// components._speaker_model_error the same way).
namespace fv::models::testing {

// Forgets the kept download failure: speaker_model_error() reads "" again.
void reset_speaker_model_error();

} // namespace fv::models::testing
