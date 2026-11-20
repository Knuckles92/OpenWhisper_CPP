#include "automation/action_copy.h"

#include <cstdint>
#include <format>

#include "automation/detail/pypath.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/url.h"
#include "base/identity.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::automation::action_copy {

namespace {

// str.splitlines(): \n, \r, \r\n, \v, \f, \x1c to \x1e, \x85, U+2028 and U+2029 end a line.
std::vector<std::string> splitlines(std::string_view text) {
    std::vector<std::string> lines;
    const std::u32string cps = py::to_code_points(text);
    std::u32string line;
    bool pending = false;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const char32_t c = cps[i];
        const bool ends = c == U'\n' || c == U'\r' || c == U'\v' || c == U'\f' || c == 0x1c ||
                          c == 0x1d || c == 0x1e || c == 0x85 || c == 0x2028 || c == 0x2029;
        if (!ends) {
            line += c;
            pending = true;
            continue;
        }
        if (c == U'\r' && i + 1 < cps.size() && cps[i + 1] == U'\n') {
            ++i;
        }
        lines.push_back(py::to_utf8(line));
        line.clear();
        pending = false;
    }
    if (pending) {
        lines.push_back(py::to_utf8(line));
    }
    return lines;
}

std::string join(const std::vector<std::string>& lines, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < lines.size() && i < count; ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

// _THE_RESULT[output] with {it}, or "" for an output that shows nothing.
std::string the_result(const Json& output, std::string_view it) {
    if (!output.is_string()) {
        return {};
    }
    const std::string_view value = output.get_ref<const std::string&>();
    if (value == Output::PASTE) {
        return std::format("pastes {} where you were", it);
    }
    if (value == Output::SCRATCHPAD) {
        return std::format("shows {} in the Scratchpad", it);
    }
    if (value == Output::NOTICE) {
        return std::format("shows {} in a notice", it);
    }
    if (value == Output::CLIPBOARD) {
        return std::format("copies {}", it);
    }
    return {};
}

std::string then(const Json& output, std::string_view it) {
    const std::string phrase = the_result(output, it);
    return phrase.empty() ? std::string() : ", then " + phrase;
}

std::string text_param(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

// urlsplit(url).hostname or "", "" when urlsplit refuses it.
std::string host_of(std::string_view url) {
    try {
        return detail::urlsplit(url).hostname().value_or("");
    } catch (const py::ValueError&) {
        return {};
    }
}

std::string agent_summary(const Action& action) {
    const std::string name = agent_name(text_param(action.param("agent")));
    if (text_param(action.param("mode")) == AgentMode::ASK) {
        const std::string phrase = the_result(action.param("output"), "the answer");
        return phrase.empty() ? std::format("Asks {} quietly", name)
                              : std::format("Asks {} and {}", name, phrase);
    }
    if (detail::truthy(action.param("new_folder"))) {
        return std::format("Opens a {} session in a new folder with your words", name);
    }
    return std::format("Opens a {} session with your words", name);
}

std::string webhook_summary(const Action& action) {
    const std::string host = host_of(text_param(action.param("url")));
    return std::format("Sends your words to {}{}", host.empty() ? "a web address" : host,
                       then(action.param("output"), "the reply"));
}

} // namespace

const Labels& KIND_ICONS() {
    static const Labels icons = {
        {ActionKind::OPEN_LINK, "world-blue.svg"}, {ActionKind::AGENT, "wand-purple.svg"},
        {ActionKind::CHANNEL, "notes-blue.svg"},   {ActionKind::WEBHOOK, "server-blue.svg"},
        {ActionKind::PROGRAM, "cpu-blue.svg"},     {ActionKind::CLIPBOARD, "copy-gray.svg"},
        {ActionKind::SCRIPTS, "bolt-green.svg"},
    };
    return icons;
}

const Labels& KIND_BLURBS() {
    static const Labels blurbs = {
        {ActionKind::OPEN_LINK, "Opens a link with your words in it"},
        {ActionKind::AGENT, "Hands your words to a coding agent on this computer"},
        {ActionKind::CHANNEL, "Sends your words into a running Claude Code session"},
        {ActionKind::WEBHOOK, "Posts your words, signed, to an address you choose"},
        {ActionKind::PROGRAM, "Runs a program with your words on its input"},
        {ActionKind::CLIPBOARD, "Copies your words, ready to paste anywhere"},
        {ActionKind::SCRIPTS,
         std::format("Tells the programs following {}'s events", identity().product)},
    };
    return blurbs;
}

const Labels& OUTPUT_LABELS() {
    static const Labels labels = {
        {Output::NONE, "Nowhere"},          {Output::PASTE, "Paste it where I was"},
        {Output::SCRATCHPAD, "Scratchpad"}, {Output::NOTICE, "A notice"},
        {Output::CLIPBOARD, "Clipboard"},
    };
    return labels;
}

const Labels& MODE_LABELS() {
    static const Labels labels = {{AgentMode::HANDOFF, "Open a session"},
                                  {AgentMode::ASK, "Quick answer"}};
    return labels;
}

const Labels& SURFACE_LABELS() {
    static const Labels labels = {{AgentSurface::AUTO, "Auto"},
                                  {AgentSurface::APP, "App"},
                                  {AgentSurface::TERMINAL, "Terminal"}};
    return labels;
}

const Labels& SURFACE_HINTS() {
    static const Labels hints = {
        {AgentSurface::AUTO,
         "The agent's app when it's installed, with your words filled in for you to send; "
         "otherwise a new terminal, where the agent starts on them right away."},
        {AgentSurface::APP,
         "The agent's app opens with your words filled in, ready for you to send."},
        {AgentSurface::TERMINAL,
         "A new terminal opens, and the agent starts on your words right away."},
    };
    return hints;
}

std::string label_for(const Labels& table, std::string_view key) {
    for (const auto& [name, value] : table) {
        if (name == key) {
            return value;
        }
    }
    return {};
}

std::string no_app_hint(std::string_view agent_name) {
    return std::format(
        "{} has no app, so it opens in a new terminal and starts on your words right away.",
        agent_name);
}

std::string agent_name(std::string_view agent_id) {
    return std::string(agent_display_name(agent_id).value_or("your agent"));
}

std::string link_destination(std::string_view template_text) {
    const std::string scheme = link_scheme(template_text);
    if (scheme == "claude") {
        return "a new Claude chat";
    }
    if (scheme == "codex") {
        return "a new ChatGPT chat";
    }
    if (scheme == "http" || scheme == "https") {
        std::string host = host_of(template_text);
        if (host.starts_with("www.")) {
            host.erase(0, 4);
        }
        if (host == "chatgpt.com") {
            return "ChatGPT in your browser";
        }
        if (host == "claude.ai") {
            return "Claude in your browser";
        }
        return host.empty() ? "a web page" : host;
    }
    // An app's own scheme, like obsidian:, names the app it opens.
    return scheme.empty() ? "a link" : py::capitalize(scheme);
}

std::string summary(const Action& action) {
    const std::string_view kind = action.kind;
    if (kind == ActionKind::OPEN_LINK) {
        return std::format("Opens {} with your words",
                           link_destination(text_param(action.param("template"))));
    }
    if (kind == ActionKind::AGENT) {
        return agent_summary(action);
    }
    if (kind == ActionKind::CHANNEL) {
        return "Sends your words to your Claude Code session";
    }
    if (kind == ActionKind::WEBHOOK) {
        return webhook_summary(action);
    }
    if (kind == ActionKind::PROGRAM) {
        std::string program = detail::basename(text_param(action.param("program")));
        if (program.empty()) {
            program = "a program";
        }
        return std::format("Runs {} with your words{}", program,
                           then(action.param("output"), "its output"));
    }
    if (kind == ActionKind::CLIPBOARD) {
        return "Copies your words without pasting them";
    }
    if (kind == ActionKind::SCRIPTS) {
        return std::format("Tells the programs listening to {}", identity().product);
    }
    return "Does something with your words";
}

std::string shortened(std::string_view text) {
    const std::string whole = join(splitlines(py::strip(text)), SIZE_MAX);
    std::string kept = join(splitlines(whole), TRY_OUTPUT_LINES);
    if (py::len(kept) > TRY_OUTPUT_CHARS) {
        kept = py::rstrip(py::slice(kept, 0, static_cast<std::int64_t>(TRY_OUTPUT_CHARS)));
    }
    return kept == whole ? kept : kept + "…";
}

std::string try_outcome(const ActionResult& result) {
    const std::string message =
        !result.message.empty() ? result.message : (result.ok ? "It worked." : "It didn't work.");
    return result.output.empty() ? message : message + "\n" + shortened(result.output);
}

std::string spoken_hint(const Action& action) {
    return action.spoken_names.empty() ? std::string()
                                       : std::format("Say “{}”", action.spoken_names.front());
}

std::vector<std::string> parse_spoken_names(std::string_view text) {
    std::vector<std::string> names;
    for (const std::string& part : py::split_on(text, ",")) {
        std::string name;
        for (const std::string& word : py::split(part)) {
            name += (name.empty() ? "" : " ") + word;
        }
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

std::vector<std::string> parse_arguments(std::string_view text) {
    std::vector<std::string> arguments;
    for (const std::string& line : splitlines(text)) {
        std::string stripped = py::strip(line);
        if (!stripped.empty()) {
            arguments.push_back(std::move(stripped));
        }
    }
    return arguments;
}

} // namespace fv::automation::action_copy
