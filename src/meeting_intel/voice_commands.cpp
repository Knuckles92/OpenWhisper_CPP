#include "meeting_intel/voice_commands.h"

#include <algorithm>
#include <format>
#include <sstream>
#include <utility>

#include "meeting_agent/typesafe_signals.h"
#include "meeting_state/corrections.h"
#include "meeting_state/fast_ops.h"
#include "pyport/text.h"

namespace fv::meeting_intel {
namespace {

std::string text(const Json& obj, std::string_view key) {
    const auto value = obj.find(key);
    return value != obj.end() && value->is_string() ? value->get<std::string>() : std::string();
}

double time_of(const Json& obj, std::string_view key, double fallback = 0) {
    const auto value = obj.find(key);
    return value != obj.end() && value->is_number() ? value->get<double>() : fallback;
}

// voice_actions.correction_op: the "replace X with Y" form, then the "it is Y, not X" form; a
// form whose pair is unusable or changes no earlier segment falls through to the next.
Json correction(const Json& row, const std::vector<Json>& old) {
    static const py::Regex patterns[] = {
        py::Regex(
            R"(\b(?:replace|change|correct)\s+(.+?)\s+(?:with|to)\s+(.+?)(?:\s+please)?[.!?]*$)",
            py::IGNORECASE),
        py::Regex(R"(\b(?:it is|it's|I said)\s+(.+?),?\s+not\s+(.+?)(?:\s+please)?[.!?]*$)",
                  py::IGNORECASE),
    };
    const auto command = py::strip(text(row, "text"));
    const auto id = text(row, "id");
    if (id.empty()) {
        return Json::array();
    }
    constexpr std::string_view trim = " \"'“”‘’.,!?";
    for (std::size_t index = 0; index < std::size(patterns); ++index) {
        const auto match = patterns[index].search(command);
        if (!match) {
            continue;
        }
        const auto first = py::strip(match->group(1), trim);
        const auto second = py::strip(match->group(2), trim);
        const auto& source = index == 0 ? first : second;
        const auto& target = index == 0 ? second : first;
        if (source.empty() || target.empty() || py::casefold(source) == py::casefold(target) ||
            std::max(py::len(source), py::len(target)) > meeting_state::MAX_TERM_CHARS) {
            continue;
        }
        std::deque<std::string> evidence;
        for (const auto& prior : old) {
            const auto priorId = text(prior, "id");
            const auto transcript = text(prior, "text");
            if (!priorId.empty() && priorId != id &&
                meeting_state::correct_text(transcript, {{py::lower(source), target}}) !=
                    transcript) {
                evidence.push_back(priorId);
                if (evidence.size() > 18) {
                    evidence.pop_front();
                }
            }
        }
        if (evidence.empty()) {
            continue;
        }
        evidence.push_back(id);
        return Json::array(
            {{{"op", "add_item"},
              {"card", "user_notes"},
              {"text", std::format("Spoken correction: “{}” → “{}”.", source, target)},
              {"evidence", evidence},
              {"data",
               {{"kind", "term_correction"},
                {"selected_text", source},
                {"replacement", target},
                {"source", "voice_command"},
                {"command", "fix_transcript"}}}}});
    }
    return Json::array();
}

// re.findall(r"\w+", text.lower()) joined by spaces (observe_preview's normalize).
std::string normalized_words(const std::string& input) {
    static const py::Regex word(R"(\w+)");
    std::string out;
    for (const auto& found : word.findall(py::lower(input))) {
        if (!out.empty()) {
            out += ' ';
        }
        out += found;
    }
    return out;
}

} // namespace

std::optional<py::Regex> compile_wake_pattern(const std::vector<std::string>& names) {
    std::string alternatives;
    for (const auto& name : names) {
        std::string pattern;
        for (const auto& word : py::split(py::lower(name))) {
            if (!pattern.empty()) {
                pattern += R"([\s\-]*)";
            }
            pattern += py::regex_escape(word);
        }
        if (!pattern.empty()) {
            if (!alternatives.empty()) {
                alternatives += '|';
            }
            alternatives += pattern;
        }
    }
    if (alternatives.empty()) {
        return std::nullopt;
    }
    return py::Regex(R"((?<![a-z0-9])(?:)" + alternatives + R"()(?![a-z0-9]))", py::IGNORECASE);
}

bool mentions_assistant(std::string_view input, const std::optional<py::Regex>& pattern) {
    return pattern && !input.empty() && pattern->search(input).has_value();
}

std::vector<Json> referent_rows(const std::vector<Json>& previous, const Json& command,
                                const std::vector<std::string>& exclude_ids) {
    const auto start = time_of(command, "start_s");
    const auto command_id = text(command, "id");
    std::vector<Json> picked;
    for (auto item = previous.rbegin(); item != previous.rend() && picked.size() < 2; ++item) {
        const auto id = text(*item, "id");
        if (id == command_id || std::ranges::find(exclude_ids, id) != exclude_ids.end() ||
            py::strip(text(*item, "text")).empty()) {
            continue;
        }
        auto end = time_of(*item, "end_s");
        if (end == 0) {
            end = time_of(*item, "start_s");
        }
        if (end > start || start - end > 20) {
            continue;
        }
        picked.push_back(*item);
    }
    std::ranges::reverse(picked);
    return picked;
}

std::optional<std::string> extract_spoken_topic(std::string_view input) {
    // voice_commands._TOPIC_LEAD_INS, _TOPIC_RE and _TOPIC_TRAILERS.
    static const py::Regex lead(
        R"((?:new topic(?: is)?|the topic is(?: now)?|topic is(?: now)?|set the topic to|)"
        R"(set topic to|we're moving on to|we are moving on to|moving on to|move on to|)"
        R"(moving to|new section for|mark a new section for|next topic(?: is)?|)"
        R"(switch(?:ing)? to)\s*[:,-]?\s*(?P<topic>.+)$)",
        py::IGNORECASE);
    static const py::Regex tail(R"(\s*(?:please|now|thanks|thank you|okay|ok)?\s*[.!?,;:]*\s*$)",
                                py::IGNORECASE);
    const auto match = lead.search(input);
    if (!match) {
        return std::nullopt;
    }
    auto output = py::strip(tail.sub("", match->group("topic")), " \"'");
    if (output.empty()) {
        return std::nullopt;
    }
    output = py::strip(py::slice(output, 0, 120));
    return output.empty() ? std::nullopt : std::optional(std::move(output));
}

namespace {

Json command_evidence(const Json& row) {
    const auto ids = row.find("_evidence_ids");
    if (ids != row.end() && ids->is_array() && !ids->empty()) {
        return *ids;
    }
    const auto id = text(row, "id");
    return id.empty() ? Json::array() : Json::array({id});
}

// _is_wake_stub: the wake name with nothing but a preamble after it.
bool wake_stub(const std::string& segment, const py::Regex& wake) {
    const auto match = wake.search(segment);
    if (!match) {
        return false;
    }
    static const py::Regex polite(R"(^(?:(?:can|could|would) you\s+)?(?:please\s+)?)");
    static const py::Regex note(R"((?:take|make|add) a note(?: that)?)");
    const auto tail =
        polite.sub("", py::lower(py::strip(segment.substr(match->byte_end()), " .,!?:;")));
    static const std::vector<std::string> stubs{
        "",          "please",    "can you",      "could you",
        "would you", "note that", "mark that as", "write down"};
    return std::ranges::find(stubs, tail) != stubs.end() || note.fullmatch(tail).has_value();
}

// _prepare: joins a wake name split from its command, and notes the dictated content.
std::optional<Json> prepare_command(Json row, const Json& previous, const py::Regex& wake) {
    auto segment = py::strip(text(row, "text"));
    if (segment.empty()) {
        return std::nullopt;
    }
    auto match = wake.search(segment);
    if (!match) {
        const auto earlier = text(previous, "text");
        const auto gap = time_of(row, "start_s") - time_of(previous, "end_s");
        if (gap < 0 || gap > 3 || text(previous, "channel") != text(row, "channel") ||
            !wake_stub(earlier, wake)) {
            return std::nullopt;
        }
        segment = py::rstrip(earlier, " .,!?:;") + ", " + segment;
        row["text"] = segment;
        row["start_s"] = time_of(previous, "start_s");
        auto evidence = command_evidence(previous);
        if (const auto id = text(row, "id"); !id.empty()) {
            evidence.push_back(id);
        }
        row["_evidence_ids"] = std::move(evidence);
        match = wake.search(segment);
        if (!match) {
            return std::nullopt;
        }
    }
    const auto prefix = py::strip(segment.substr(0, match->byte_start()), " .,!?:;");
    const auto folded = py::lower(prefix);
    if (folded != "hey" && folded != "okay" && folded != "ok" && folded != "hi") {
        row["_prefix"] = prefix;
    }
    // voice_commands._INLINE_CONTENT_RE.
    static const py::Regex inline_content(
        R"(^(?:(?:can|could|would) you\s+)?(?:please\s+)?(?:note that|note this:|remember that|)"
        R"((?:take|make|add) a note(?: that)?|)"
        R"(add (?:an action item|a decision)(?: that)?|write down|capture this:))"
        R"(\s*[.:,-]?\s+(.+)$)",
        py::IGNORECASE);
    // Only the separator after the wake name goes: dictated content keeps its final period.
    const auto spoken = py::strip(py::lstrip(segment.substr(match->byte_end()), " .,!?:;"));
    if (const auto content = inline_content.search(spoken)) {
        row["_content"] = py::strip(content->group(1));
    }
    return row;
}

} // namespace

Json voice_command_ops(std::string_view command, const Json& row, const std::vector<Json>& prior,
                       const std::vector<std::string>& exclude_ids) {
    const auto id = text(row, "id");
    if (id.empty()) {
        return Json::array();
    }
    const auto command_ids = command_evidence(row);
    // voice_actions.correction_op and build_ops cite the command row alone for these two; only
    // a card item carries a split wake's stub rows.
    if (command == "fix_transcript") {
        return correction(row, prior);
    }
    if (command == "set_topic") {
        const auto topic = extract_spoken_topic(text(row, "text"));
        return topic
                   ? Json::array(
                         {{{"op", "set_topic"}, {"text", *topic}, {"evidence", Json::array({id})}}})
                   : Json::array();
    }
    const std::string_view card = command == "mark_decision" ? "decisions"
                                  : command == "mark_action" ? "action_items"
                                  : command == "note_this"   ? "live_notes"
                                                             : "";
    if (card.empty()) {
        return Json::array();
    }
    auto excluded = exclude_ids;
    for (const auto& cited : command_ids) {
        if (cited.is_string()) {
            excluded.push_back(cited.get<std::string>());
        }
    }
    const auto referents = referent_rows(prior, row, excluded);
    // Explicit dictated content, or a point said before the wake name in the same segment, takes
    // precedence over unrelated earlier transcript rows.
    std::string body = text(row, "_content");
    if (body.empty()) {
        body = text(row, "_prefix");
    }
    const bool copied = !body.empty();
    auto evidence = Json::array();
    if (!copied) {
        for (const auto& item : referents) {
            if (!body.empty()) {
                body += " ";
            }
            body += py::strip(text(item, "text"));
            if (auto cited = text(item, "id"); !cited.empty()) {
                evidence.push_back(std::move(cited));
            }
        }
        body = py::strip(body);
    }
    const double anchor = copied || referents.empty() ? time_of(row, "start_s")
                                                      : time_of(referents.front(), "start_s");
    if (body.empty()) {
        return Json::array();
    }
    for (const auto& cited : command_ids) {
        evidence.push_back(cited);
    }
    Json data{{"source", "voice_command"}, {"command", command}, {"command_segment_id", id}};
    if (card == "live_notes") {
        data["heading"] = "Requested note";
        data["start_s"] = anchor;
    }
    return Json::array({{{"op", "add_item"},
                         {"card", card},
                         {"text", std::move(body)},
                         {"data", std::move(data)},
                         {"evidence", std::move(evidence)}}});
}

VoiceCommandListener::VoiceCommandListener(std::shared_ptr<meeting_state::MeetingStateStore> store,
                                           std::vector<std::string> names, Deps deps)
    : store_(std::move(store)), names_(std::move(names)), wake_(compile_wake_pattern(names_)),
      deps_(std::move(deps)) {}

VoiceCommandListener::~VoiceCommandListener() {
    shutdown();
}

void VoiceCommandListener::shutdown() {
    closed_.cancel();
    worker_.shutdown(std::chrono::seconds(5));
}

int VoiceCommandListener::observe(const std::vector<Json>& rows) {
    if (!wake_ || closed_.cancelled()) {
        return 0;
    }
    int queued = 0;
    // Feedback is said under the lock so "heard" always precedes the worker's answer.
    std::lock_guard lock(mutex_);
    for (auto row : rows) {
        auto& preview = previews_[text(row, "channel")];
        preview.committed_end = std::max(preview.committed_end, time_of(row, "end_s"));
        if (time_of(preview.last, "start_s") <= preview.committed_end) {
            ++preview.generation;
            preview.pending.reset();
            preview.last = Json();
            preview.raw_text.clear();
        }
        const auto id = text(row, "id");
        if (id.empty() || py::strip(text(row, "text")).empty() ||
            std::ranges::any_of(recent_,
                                [&id](const auto& seen) { return id == text(seen, "id"); })) {
            continue;
        }
        const auto same_channel =
            std::find_if(recent_.rbegin(), recent_.rend(), [&](const auto& item) {
                return text(item, "channel") == text(row, "channel");
            });
        const auto prepared =
            prepare_command(row, same_channel == recent_.rend() ? Json() : *same_channel, *wake_);
        const bool stub = prepared && wake_stub(text(*prepared, "text"), *wake_);
        if (stub) {
            feedback("heard", "Heard you — listening for the rest…", "");
        } else if (prepared && std::ranges::find(handled_, id) == handled_.end()) {
            handled_.push_back(id);
            if (handled_.size() > 64) {
                handled_.pop_front();
            }
            handled_rows_.push_back(*prepared);
            if (handled_rows_.size() > 32) {
                handled_rows_.pop_front();
            }
            feedback("heard", "Heard you…", "");
            if (pending_.load() >= 16) {
                feedback("error", "Voice commands are busy. Please repeat that shortly.", "");
            } else {
                // Earlier commands and the command's own rows are never what "that" names.
                std::vector<std::string> excluded(handled_.begin(), handled_.end());
                for (const auto& cited : command_evidence(*prepared)) {
                    if (cited.is_string()) {
                        excluded.push_back(cited.get<std::string>());
                    }
                }
                std::vector<Json> previous(recent_.begin(), recent_.end());
                std::ranges::stable_sort(previous, {},
                                         [](const Json& item) { return time_of(item, "start_s"); });
                ++pending_;
                ++queued;
                worker_.post([this, command = *prepared, previous = std::move(previous),
                              excluded = std::move(excluded)] {
                    struct Finish {
                        std::atomic<int>& pending;
                        ~Finish() { --pending; }
                    } finish{pending_};
                    run(command, previous, excluded);
                });
            }
        }
        recent_.push_back(stub ? *prepared : std::move(row));
        if (recent_.size() > 24) {
            recent_.pop_front();
        }
    }
    return queued;
}

bool VoiceCommandListener::allowed() const {
    return !closed_.cancelled() && deps_.allowed && deps_.allowed();
}

void VoiceCommandListener::observe_preview(const Json& row) {
    if (!wake_ || closed_.cancelled()) {
        return;
    }
    const auto segment = text(row, "text");
    if (segment.empty()) {
        return;
    }
    const std::lock_guard lock(mutex_);
    auto& preview = previews_[text(row, "channel")];
    if (time_of(row, "end_s") <= preview.committed_end || preview.raw_text == segment) {
        return;
    }
    auto prepared = prepare_command(row, preview.last, *wake_);
    if (prepared) {
        // Rolling windows repeat already saved commands: the bubble is not reopened, and an
        // in-flight preview is left as it is.
        const auto channel = text(row, "channel");
        const auto words = normalized_words(text(*prepared, "text"));
        const auto start = time_of(*prepared, "start_s");
        if (std::ranges::any_of(handled_rows_, [&](const Json& handled) {
                return text(handled, "channel") == channel && time_of(handled, "end_s") >= start &&
                       words.find(normalized_words(text(handled, "text"))) != std::string::npos;
            })) {
            preview.last = *prepared;
            preview.raw_text = segment;
            return;
        }
    }
    ++preview.generation;
    const auto was_pending = preview.pending.has_value();
    preview.pending.reset();
    preview.last = prepared ? *prepared : row;
    preview.raw_text = segment;
    if (!prepared) {
        return;
    }
    if (!was_pending) {
        preview.queue_order = ++preview_sequence_;
    }
    preview.pending = std::move(*prepared);
    feedback("heard", "Heard you — listening…", "");
    if (!preview_running_) {
        preview_running_ = true;
        worker_.post([this] { run_preview(); });
    }
}

void VoiceCommandListener::run_preview() {
    std::string channel;
    std::uint64_t generation = 0;
    Json row;
    std::vector<Json> previous;
    {
        const std::lock_guard lock(mutex_);
        const auto selected = std::ranges::min_element(previews_, [](const auto& a, const auto& b) {
            if (a.second.pending.has_value() != b.second.pending.has_value()) {
                return a.second.pending.has_value();
            }
            return a.second.queue_order < b.second.queue_order;
        });
        if (selected == previews_.end() || !selected->second.pending || closed_.cancelled()) {
            preview_running_ = false;
            return;
        }
        channel = selected->first;
        generation = selected->second.generation;
        row = std::move(*selected->second.pending);
        selected->second.pending.reset();
        previous.assign(recent_.begin(), recent_.end());
    }
    // Queue the next preview behind any committed work already waiting.
    worker_.post([this] { run_preview(); });
    judge_preview(channel, generation, row, previous);
}

void VoiceCommandListener::judge_preview(const std::string& channel, std::uint64_t generation,
                                         const Json& row, const std::vector<Json>& previous) {
    if (closed_.cancelled()) {
        return;
    }
    const auto judge = deps_.judge && allowed() ? deps_.judge() : nullptr;
    if (!judge || !judge->is_local() || !allowed()) {
        feedback("unavailable", VOICE_NEEDS_LOCAL_JUDGE, "");
        return;
    }
    const auto segment = text(row, "text");
    if (wake_stub(segment, *wake_)) {
        return;
    }
    std::vector<std::string> context;
    for (const auto& item : previous) {
        context.push_back(text(item, "text"));
    }
    std::optional<llm::ChoiceAnswer> choice;
    try {
        choice =
            meeting_agent::voice_command_choice(*judge, segment, context, names_, closed_.token());
    } catch (const std::exception&) {
        return;
    }
    const bool still_allowed = allowed();
    // The freshness check and the feedback stay ordered with commits, so a saved message is
    // never overtaken by an obsolete preview.
    const std::lock_guard lock(mutex_);
    if (previews_.at(channel).generation != generation || !still_allowed) {
        return;
    }
    if (!choice) {
        feedback("error", "Voice recognition is unavailable. Please try again.", "");
    } else if (choice->choice != "none" &&
               choice->confidence >= meeting_agent::VOICE_COMMAND_CONFIDENCE) {
        feedback("recognized", "Got it — waiting for the transcript…", choice->choice);
    }
}

namespace {

std::string_view working_message(std::string_view command) {
    if (command == "mark_decision") {
        return "Recording the decision…";
    }
    if (command == "mark_action") {
        return "Adding an action item…";
    }
    if (command == "note_this") {
        return "Taking a note…";
    }
    if (command == "set_topic") {
        return "Updating the topic…";
    }
    if (command == "recap") {
        return "Preparing a recap…";
    }
    if (command == "fix_transcript") {
        return "Correcting the transcript…";
    }
    return "Working on it…";
}

std::string_view saved_message(std::string_view command) {
    if (command == "mark_decision") {
        return "Decision noted";
    }
    if (command == "mark_action") {
        return "Action item added";
    }
    if (command == "note_this") {
        return "Added to Meeting Notes";
    }
    if (command == "set_topic") {
        return "Topic updated";
    }
    return "Saved";
}

} // namespace

void VoiceCommandListener::run(Json row, const std::vector<Json>& previous,
                               const std::vector<std::string>& excluded) {
    if (closed_.cancelled() || !store_) {
        return;
    }
    const auto judge = deps_.judge && allowed() ? deps_.judge() : nullptr;
    if (!judge || !judge->is_local() || !allowed()) {
        feedback("unavailable", VOICE_NEEDS_LOCAL_JUDGE, "");
        return;
    }
    std::vector<std::string> context;
    for (const auto& item : previous) {
        context.push_back(text(item, "text"));
    }
    std::optional<llm::ChoiceAnswer> choice;
    try {
        choice = meeting_agent::voice_command_choice(*judge, text(row, "text"), context, names_,
                                                     closed_.token());
    } catch (const std::exception&) {
        feedback("error", "Could not recognize the command. Please try again.", "");
        return;
    }
    if (!allowed()) {
        return;
    }
    if (!choice) {
        feedback("error", "Voice recognition is unavailable. Please try again.", "");
        return;
    }
    if (choice->choice == "none" || choice->confidence < meeting_agent::VOICE_COMMAND_CONFIDENCE) {
        feedback("uncertain", "No command saved. Try ‘assistant, note that…’.", "");
        return;
    }
    feedback("working", working_message(choice->choice), choice->choice);
    if (choice->choice == "recap") {
        if (deps_.recap) {
            deps_.recap(text(row, "text"));
        } else {
            feedback("error", "This command is currently unavailable.", choice->choice);
        }
        return;
    }
    if (choice->choice == "fix_transcript") {
        apply_correction(row, previous);
        return;
    }
    const auto ops = voice_command_ops(choice->choice, row, previous, excluded);
    if (ops.empty()) {
        feedback("uncertain", "Nothing saved yet. Say the point you want me to capture.",
                 choice->choice);
        return;
    }
    if (!allowed()) {
        return;
    }
    std::vector<meeting_state::OpResult> applied;
    try {
        applied = store_->apply("system", std::string(meeting_state::VOICE_COMMAND_ACTOR), ops);
    } catch (const std::exception&) {
        feedback("error", "The command could not be saved. Please try again.", choice->choice);
        return;
    }
    if (std::ranges::any_of(applied, [](const auto& result) { return result.ok; })) {
        feedback("saved", saved_message(choice->choice), choice->choice);
    } else {
        feedback("error", "The command could not be saved.", choice->choice);
    }
    if (deps_.applied && !closed_.cancelled()) {
        deps_.applied(choice->choice, applied);
    }
}

// engine._apply_spoken_action("fix_transcript"): a reversible, source-backed term correction.
void VoiceCommandListener::apply_correction(const Json& row, const std::vector<Json>& previous) {
    const auto ops = voice_command_ops("fix_transcript", row, previous);
    if (ops.empty()) {
        feedback("uncertain",
                 "Correction not applied. Say “note taker, replace X with Y” using a term in the "
                 "recent transcript.",
                 "fix_transcript");
        return;
    }
    if (!allowed()) {
        return;
    }
    std::vector<meeting_state::OpResult> applied;
    try {
        applied = store_->apply("system", std::string(meeting_state::VOICE_COMMAND_ACTOR), ops);
    } catch (const std::exception&) {
        feedback("error", "The transcript correction could not be saved.", "fix_transcript");
        return;
    }
    if (deps_.applied && !closed_.cancelled()) {
        deps_.applied("fix_transcript", applied);
    }
    if (std::ranges::any_of(applied, [](const auto& result) { return result.ok; })) {
        feedback("saved",
                 "Transcript correction applied. Remove the spoken correction note to undo it.",
                 "fix_transcript");
    } else {
        feedback("error", "The transcript correction could not be saved.", "fix_transcript");
    }
}

void VoiceCommandListener::feedback(std::string_view phase, std::string_view message,
                                    std::string_view command) {
    if (deps_.feedback && !closed_.cancelled()) {
        deps_.feedback(Json{{"phase", phase}, {"message", message}, {"command", command}});
    }
}

} // namespace fv::meeting_intel
