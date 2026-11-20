#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

// Startup recovery of interrupted dictations (services/runtime/transcription.py
// recover_recordings).
namespace fv::dictation {

// The status line after startup recovery: "Recovered {n} walkthrough(s) — open Saved
// Walkthroughs to finish them" (walkthroughs win), "Recovered {n} interrupted recording(s) in
// Recordings", or "" when nothing was recovered. Thread-safe.
std::string recovery_status(std::size_t recordings, std::size_t walkthroughs);

// Turns the journals a crash left beside `output_file` into WAVs in `recordings_folder`
// (recorder's recover_recordings) and returns the status to show. The orphaned preview audio
// arrives with its feature (M2). Never throws: a failure is logged
// and recovers nothing. Blocks on disk: a worker thread.
std::string recover_interrupted_recordings(const std::filesystem::path& output_file,
                                           const std::filesystem::path& recordings_folder);

// The same, then the walkthrough half: each interrupted walkthrough under `captures_root` is
// joined to the WAV recovered from its journal (walkthrough::attach_recovered_audio), so Saved
// Walkthroughs can finish it, and the expired walkthroughs are pruned (walkthrough::prune).
// Neither loads the document renderer. Never throws; blocks on disk: a worker thread.
std::string recover_interrupted_recordings(const std::filesystem::path& output_file,
                                           const std::filesystem::path& recordings_folder,
                                           const std::filesystem::path& captures_root);

} // namespace fv::dictation
