#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "walkthrough/session.h"

// The walkthrough job of the transcription runtime (services/runtime/transcription.py: the
// walkthrough's start, its section-by-section decode, its delivery and its AI write-up). The
// runtime's members that do it are in walkthrough_job.cpp; this header holds what a caller
// passes in.
namespace fv::dictation {

// One AI write-up of a finished walkthrough: _start_write_up's write_up.write_up call, with what
// it reads from the settings at the end of the job.
// Affinity: value type.
struct WalkthroughWriteUpRequest {
    // The walkthrough as spoken, its documents written.
    walkthrough::WalkthroughResult result;
    // Its screenshots in the board's order, and the notes held for the agent.
    std::vector<std::filesystem::path> shots;
    std::vector<walkthrough::Note> notes;
    // walkthrough_write_up_agent, walkthrough_write_up_instructions ("" for the default words)
    // and walkthrough_write_up_notes.
    std::string agent;
    std::string instructions;
    bool follow_notes = false;
    // The settings the job ended with (the documents' paper).
    Json settings = Json::object();
    // The write-up's own cancel (cancel_write_up and quitting), never the job's.
    CancelToken cancel;
};

// Writes a walkthrough up; anything it throws becomes the result's write_up_error and the
// walkthrough is copied as spoken. Runs on the walkthrough-library strand.
using WalkthroughWriteUp =
    std::function<walkthrough::WalkthroughResult(const WalkthroughWriteUpRequest&)>;

// walkthrough::write_up::write_up with the request's values and the installed agents (the
// agent task services the composition sets). Throws write_up::WriteUpFailed.
walkthrough::WalkthroughResult write_up_walkthrough(const WalkthroughWriteUpRequest& request);

} // namespace fv::dictation
