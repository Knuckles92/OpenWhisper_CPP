#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "walkthrough/document.h"
#include "walkthrough/session.h"

// Turning a stopped walkthrough into its words and documents (services/walkthrough/finish.py);
// job worker thread. Each section is transcribed on its own, through the same call a dictation
// uses, so no engine needs to report timestamps.
namespace fv::walkthrough {

// Decodes one WAV clip to text.
using ClipTranscriber = std::function<std::string(const std::filesystem::path& wav)>;
// Called with (index, total), from 1, before each clip is decoded; it may throw to stop (a
// Cancel), and the exception leaves transcribe_sections as it is.
using BeforeEach = std::function<void(int index, int total)>;

// The words of each section, one more than there are screenshots, and the held notes with the
// section each was said in. The clips are written to a new temporary folder
// ("<product>-walkthrough-*", removed afterwards). When the recording cannot be read or cut
// (OSError, ValueError or wave.Error in Python: std::system_error, std::invalid_argument,
// WaveError), it is decoded whole into the first section after before_each(1, 1), the other
// sections are "" and there are no notes.
std::pair<std::vector<std::string>, std::vector<Note>>
transcribe_sections(const std::filesystem::path& audio, const Storyboard& board,
                    const ClipTranscriber& transcribe, const BeforeEach& before_each = {});

// The sections' words with "[Screenshot N]" where each screenshot sits, joined by blank lines.
std::string marked_text(std::span<const std::string> texts, int shots);

// The saved format and paper of the documents (walkthrough_title, walkthrough_show_details,
// walkthrough_show_about, walkthrough_show_captions, walkthrough_page_size) in a settings
// snapshot; a page size other than "letter" or "a4" is the locale's (PaperSize::Auto).
std::pair<DocumentFormat, PaperSize> document_settings(const Json& settings);

// Writes the walkthrough's Markdown, PDF, Word document and manifest into the session's folder,
// in that order, then deletes draft.json. `cleaned` and `raw` hold one text per screenshot and
// then the words after the last one; `notes` go into the manifest only, with the section they
// were said in. The PDF and Word document come from document_renderer(). Throws DocumentError
// from the renderer, std::filesystem::filesystem_error when a file cannot be written, and
// std::out_of_range when `cleaned` has fewer than shots.size() + 1 texts.
WalkthroughResult build_documents(WalkthroughSession& session, std::span<const Shot> shots,
                                  std::span<const std::string> cleaned,
                                  std::span<const std::string> raw, double duration_s,
                                  std::span<const Note> notes, const Json& settings);

// Where a failed walkthrough's screenshots are, for its error status
// (" — screenshots kept in <folder>"), or "" without a session or screenshots.
std::string kept_note(const WalkthroughSession* session);

} // namespace fv::walkthrough
