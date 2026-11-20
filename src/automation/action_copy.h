#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "automation/action_model.h"
#include "automation/action_result.h"

// What the Actions page says about an action: icons, labels, one-line summaries
// (ui_qt/widgets/action_copy.py). Plain functions with no Qt, so the cards and the editor phrase
// an action the same way. Product names come from identity(). Every function is thread-safe.
namespace fv::automation::action_copy {

// Words "Try it" and the link preview start with.
inline constexpr std::string_view SAMPLE_WORDS = "What's on my calendar tomorrow?";
// The most of an answer or a program's output "Try it" shows.
inline constexpr std::size_t TRY_OUTPUT_LINES = 6;
inline constexpr std::size_t TRY_OUTPUT_CHARS = 500;

using Labels = std::vector<std::pair<std::string_view, std::string>>;

// (kind, icon file) for every ActionKind, in ActionKind::ALL order.
const Labels& KIND_ICONS();
// (kind, what it does in a line) under the editor's title.
const Labels& KIND_BLURBS();
// (output, label) as the choosers show where an answer, reply or output goes.
const Labels& OUTPUT_LABELS();
const Labels& MODE_LABELS();
const Labels& SURFACE_LABELS();
// (surface, where a session opens and whether the agent waits for you there).
const Labels& SURFACE_HINTS();
// The value for `key` in one of the tables above, or "" when it has none.
std::string label_for(const Labels& table, std::string_view key);
// NO_APP_HINT with the agent's name: "{name} has no app, so it opens in a new terminal and
// starts on your words right away."
std::string no_app_hint(std::string_view agent_name);

// AGENT_NAMES[agent_id], else "your agent".
std::string agent_name(std::string_view agent_id);
// Where a link template goes, for "Opens …": "a new Claude chat", "a new ChatGPT chat",
// "ChatGPT in your browser", "Claude in your browser", a web host without "www.", "a web page",
// an app scheme capitalized ("Obsidian"), or "a link".
std::string link_destination(std::string_view template_text);
// One line on what `action` does with the words, without a full stop.
std::string summary(const Action& action);
// `text` cut to 6 lines and 500 code points, ending in "…" when any was left out.
std::string shortened(std::string_view text);
// What "Try it" says about a result: what happened, then the answer, shortened.
std::string try_outcome(const ActionResult& result);
// "Say “Claude”" for the first spoken name, or "".
std::string spoken_hint(const Action& action);
// A comma-separated list of spoken names, whitespace runs collapsed, empty ones dropped.
std::vector<std::string> parse_spoken_names(std::string_view text);
// Program arguments, one per line; blank lines dropped, each line stripped.
std::vector<std::string> parse_arguments(std::string_view text);

} // namespace fv::automation::action_copy
