#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "walkthrough/document.h"
#include "walkthrough/session.h"
#include "walkthrough/write_up.h"

// Saved walkthroughs (services/walkthrough/library.py:100-472): loading one back, Review, the
// write-up's versions and edits, recovery after a crash, and opt-in retention. Only local,
// regular files directly inside a capture are accepted (files.h). Review removes images from the
// documents, not the original PNGs, so a correction is reversible. Recovery, attaching recovered
// audio and pruning never touch the document renderer (renderer.h), so startup can run them
// before it exists. Every function blocks on disk I/O: worker threads only.
//
// Errors are Python's: fv::py::ValueError with the message to show (the texts below), and
// std::filesystem::filesystem_error where Python raised OSError. A manifest missing a key reads
// as fv::py::PyError "KeyError", a value of the wrong kind as fv::py::TypeError.
namespace fv::walkthrough {

// The walkthrough saved in folder/walkthrough.json, its screenshots resolved inside the folder.
// ValueError "Unsupported walkthrough version", "Invalid screenshot name", "Invalid walkthrough
// notes", "Invalid walkthrough time", and local_file's and read_json's texts. A screenshot Review
// left out whose PNG was deleted by hand reads as none. A start with a UTC offset is read in this
// computer's zone; a naive one as this computer's wall time.
Walkthrough load_document(const std::filesystem::path& folder);

// The notes held for the agent, each before the next screenshot the documents show.
std::vector<Note> held_notes(const Walkthrough& walkthrough);

// Whether `folder`'s documents are an AI write-up (write-up.pdf and write-up.md are files).
bool written_up(const std::filesystem::path& folder);

// The Markdown, PDF and Word names of write-up `version`; nullopt is the latest.
std::array<std::string, 3> write_up_names(std::optional<int> version = std::nullopt);

// The earlier write-ups kept in `folder` (write-up-<n>.md and .pdf both there), oldest first;
// none unless the folder is written up. The latest, write-up.md, follows the last of them.
std::vector<int> earlier_write_ups(const std::filesystem::path& folder);

// `folder`'s walkthrough: its latest write-up, an earlier `version`, or as spoken (`spoken`, or
// when it has no write-up). The clipboard text is the Markdown with each image of ours linked by
// its full path on this computer and every other image removed; the history text marks each
// screenshot shown with "[Screenshot N]"; history_raw is always nullopt.
WalkthroughResult load_result(const std::filesystem::path& folder, bool spoken = false,
                              std::optional<int> version = std::nullopt);

// The walkthrough folders under `root` (named for their start, holding a manifest or a draft),
// newest first; none when root is missing or a link.
std::vector<std::filesystem::path> entries(const std::filesystem::path& root);

// Review's save: rebuilds all four documents from `texts` and `keep`, which follow the sections
// as Review shows them, `order` naming the saved section each one is (empty: the saved order),
// so a moved screenshot takes the words that led to it along. A screenshot left out stays in the
// manifest as omitted, so it can come back. The documents are rendered in a ".review-*" folder
// inside `folder` first and published together; the first save keeps the manifest as it was in
// walkthrough.original.json. Every write-up stays as it is. Returns load_result(spoken). Throws
// ValueError "The document changed; reopen Review" when the shapes don't match the saved
// sections, and the renderer's DocumentError, after which the documents are as they were.
WalkthroughResult save_review(const std::filesystem::path& folder,
                              std::span<const std::string> texts, const std::vector<bool>& keep,
                              std::span<const int> order, const Json& settings);

// A write-up's Markdown cut after each screenshot that has a line of its own: the words before
// it and that line as written, indent included; words after the last screenshot make a last part
// whose line is "".
std::vector<std::pair<std::string, std::string>> write_up_parts(std::string_view text);
// The Markdown write_up_parts cut, in the parts' (possibly new) order.
std::string join_write_up(std::span<const std::pair<std::string, std::string>> parts);
// The screenshot a write-up `line` shows, when it is one of `folder`'s.
std::optional<std::filesystem::path> write_up_shot(const std::filesystem::path& folder,
                                                   std::string_view line);

// Rebuilds write-up `version` (nullopt: the latest) from edited Markdown, trimmed, with the usual
// look. ValueError "This walkthrough has no write-up to save" when there is none, or `version` is
// not one of earlier_write_ups; write_up::WriteUpFailed when its documents can't be written.
WalkthroughResult save_write_up(const std::filesystem::path& folder, std::string_view text,
                                std::optional<int> version, const Json& settings);

// Writes `folder` up again from its as-spoken document: with `instructions` (trimmed), or the
// saved walkthrough_write_up_instructions when nullopt; with the saved agent and notes choice,
// and the notes held while recording. `run` as write_up::write_up.
WalkthroughResult retry_write_up(const std::filesystem::path& folder,
                                 std::optional<std::string> instructions, const Json& settings,
                                 const CancelToken& cancel, write_up::AgentTaskRunner run = {});

// Joins each draft under `root` without a manifest or audio to its journal's recovered WAV in
// `recordings` (exactly one recording_*-recovered-<journal_id>.wav) as audio.wav, through
// .audio.tmp; a folder that cannot be read or joined is skipped. Idempotent: a later restart joins
// what an earlier one could not. Returns how many were joined.
int attach_recovered_audio(const std::filesystem::path& root,
                           const std::filesystem::path& recordings);

// The closed session of the draft in `folder`, with only its screenshots whose PNG is whole and
// its held stretches, ready to be transcribed. ValueError "Unsupported draft version", "Invalid
// screenshot name", "Unordered screenshot times", "Unordered note times", "Invalid walkthrough
// time", and local_file's when audio.wav is missing.
std::unique_ptr<WalkthroughSession> recovery_session(const std::filesystem::path& folder);

// How many days captures under `root` are kept (captures/retention.json); 0, forever, when unset,
// unreadable or not a whole number from 0 to 3650.
int retention_days(const std::filesystem::path& root);
// Saves it, creating root. ValueError "Keep captures for 0–3650 days; 0 means forever".
void set_retention(const std::filesystem::path& root, int days);
// The finished walkthroughs (a manifest, no draft) whose manifest is older than the retention
// allows at `now` (seconds since the epoch; the current time when nullopt).
std::vector<std::filesystem::path> expired_entries(const std::filesystem::path& root,
                                                   std::optional<double> now = std::nullopt);
// Deletes a walkthrough folder, refusing links and anything a walkthrough doesn't write.
// ValueError "Not a walkthrough folder", "Folder contains extra files; remove it in your file
// manager".
void delete_entry(const std::filesystem::path& folder);
// Deletes every expired walkthrough that delete_entry accepts; how many.
int prune(const std::filesystem::path& root);

} // namespace fv::walkthrough
