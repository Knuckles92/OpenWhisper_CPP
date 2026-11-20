#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/drivers.h"
#include "base/cancel.h"
#include "base/json.h"
#include "walkthrough/document.h"
#include "walkthrough/session.h"
#include "walkthrough/write_up_names.h"

// A walkthrough written up by the user's own coding agent (services/walkthrough/write_up.py);
// job worker thread. The as-spoken documents are written first and always kept. The agent then
// replies with a document in Markdown, which becomes write-up.md, write-up.pdf and
// write-up.docx, what the user gets. How it sees the walkthrough depends on what it can be held
// to: Claude Code reads walkthrough.pdf (and the screenshots beside it), fenced to the
// walkthrough's folder; Codex and OpenCode can't be fenced to one folder, so they read no files:
// the prompt carries walkthrough.md and the screenshots are attached to it.
//
// The instructions are loose on purpose: the agent decides the layout, and only the way it names
// screenshots is fixed, so the PDF can show them. Notes the user held Hold for note to say are
// not in the walkthrough at all: they come after it in the message, each with where it was said.
//
// The names live in namespace write_up (write_up_names.h holds its file names), so the entry
// point is write_up::write_up.
namespace fv::walkthrough::write_up {

// The hard wall and the silence limit of the agent's run, in seconds. Reading a long PDF can go
// quiet for a while before the reply starts.
inline constexpr double TIMEOUT_S = 300.0;
inline constexpr double STALL_S = 150.0;

// The write-up could not be made; what() says why, for the status line.
class WriteUpFailed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One call of agents::run_reading_task (reading: Claude Code, fenced to `folder`) or
// agents::run_viewing_task (Codex and OpenCode, which see `images`). The model is always the
// agent's own default and the effort agents::FINAL_EFFORT.
struct AgentTaskRequest {
    std::string agent;
    bool reading = true;
    std::string system_prompt;
    std::string user_prompt;
    std::filesystem::path folder;              // reading only: the walkthrough's folder
    std::vector<std::filesystem::path> images; // viewing only: each screenshot's real path
    double timeout_s = TIMEOUT_S;
    double stall_s = STALL_S;
    CancelToken cancel;
};

// Runs one task: the seam tests stand in for. It may throw agents::AgentUnavailable.
using AgentTaskRunner = std::function<agents::PassOutcome(const AgentTaskRequest&)>;

// `result` with the write-up as its document; the as-spoken files stay. `shots` are its
// screenshots, in order; `agent` one of the installed agents (claude_code, codex, opencode_cli);
// `instructions` what to make of it ("" for DEFAULT_INSTRUCTIONS); `follow_notes` whether the
// agent follows notes the user spoke to it; `notes` those held for it, which it always follows.
// `settings` gives the documents' paper. A write-up already there becomes the next earlier
// version (write-up-<n>.*) in the same publication. `run` empty runs the real agent
// (agents::run_reading_task or run_viewing_task, whose services the composition sets).
// Throws WriteUpFailed when the agent is missing, failed, was canceled, sent back nothing
// usable, or the documents could not be written; std::out_of_range for an unknown agent.
WalkthroughResult write_up(const WalkthroughResult& result,
                           std::span<const std::filesystem::path> shots, std::string_view agent,
                           std::string_view instructions, bool follow_notes,
                           std::span<const Note> notes, const Json& settings,
                           const CancelToken& cancel, AgentTaskRunner run = {});

// What the status line and Settings call `agent` ("Claude Code"); std::out_of_range for an
// unknown one.
std::string agent_name(std::string_view agent);

// The agent's reply as a document and the look it asked for. The document is unfenced, without
// the look block, and its images are only our screenshots (`names`): an image named any other way
// (another file, a web address) is dropped, so nothing outside the folder is ever loaded, and a
// path to one of ours becomes its bare file name. "" when no word character is left.
std::pair<std::string, Look> document_text(std::string_view reply,
                                           std::span<const std::string> names);

// The look block at the top of `text`, read, and the text without it (_split_look). Lines it
// doesn't know are skipped; British spelling is read too.
std::pair<Look, std::string> split_look(std::string_view text);

// The agent's instructions for this run (the charter with how it sees the walkthrough and the
// rule for spoken and held notes).
std::string system_prompt(bool reading, bool follow_notes, bool held_notes);

// The notes held for the agent, each with where it was said, or "" without notes (_notes).
std::string notes_text(std::span<const Note> notes, std::span<const std::string> names);

// Claude Code's message: the instructions, where the PDF is, its screenshots and its length.
std::string reading_prompt(std::string_view instructions, const WalkthroughResult& result,
                           std::span<const std::string> names);
// Codex's and OpenCode's message: the instructions, then walkthrough.md itself. WriteUpFailed
// "could not read walkthrough.md: <why>" when it cannot be read.
std::string viewing_prompt(std::string_view instructions, const WalkthroughResult& result,
                           std::span<const std::string> names);

// Publishes `text` as write-up `version` (nullopt: the latest) with `look`: write-up.md (text and
// one "\n"), its PDF and its Word document, rendered in a ".write-up-*" folder inside the
// walkthrough's and published together. With `keep_latest` the latest write-up there becomes the
// newest earlier version in the same publication, so none is lost. WriteUpFailed "could not
// write its documents: <why>" when a file cannot be written (_write).
WalkthroughResult write(const WalkthroughResult& result, std::string_view text, const Look& look,
                        const Json& settings, std::optional<int> version = std::nullopt,
                        bool keep_latest = false);

} // namespace fv::walkthrough::write_up
