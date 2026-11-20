#include "ui_widgets/agent_picker_text.h"

#include <format>
#include <string>
#include <vector>

#include "base/identity.h"

namespace fv::agent_picker {

namespace {

// Every occurrence of `from` in text replaced by `to` (str.replace).
std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return text;
    }
    std::string out;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(from, start);
        if (at == std::string::npos) {
            out.append(text, start, std::string::npos);
            return out;
        }
        out.append(text, start, at - start);
        out.append(to);
        start = at + from.size();
    }
}

bool found(const agents::ScanResult& agents, std::string_view agent_id) {
    const auto it = agents.find(std::string(agent_id));
    return it != agents.end() && it->second.has_value();
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out.append(separator);
        }
        out.append(part);
    }
    return out;
}

} // namespace

TileState idle_state() {
    return {std::string(kIdle), "", "", false, false};
}

TileState builtin_state() {
    return {std::string(kBuiltinTone), "Built in", std::string(kBuiltinDetail), true, true};
}

TileState cleanup_builtin_state() {
    return {std::string(kBuiltinTone), "Built in", "The cleanup model below", true, true};
}

std::string command_text(std::string_view text) {
    // re.sub(r"`([^`]+)`", "“\1”"): a backtick followed at once by another starts no match, so
    // the scan moves on to the second one.
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '`') {
            const std::size_t close = text.find('`', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                out.append("“");
                out.append(text.substr(i + 1, close - i - 1));
                out.append("”");
                i = close + 1;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

TileState describe_agent(std::string_view agent_id,
                         const std::optional<agents::InstalledAgent>& agent) {
    if (!agent) {
        return {std::string(kMissing), "Not installed", "", false, false};
    }
    if (!agent->problem.empty()) {
        return {std::string(kWarning), "Update needed", command_text(agent->problem), false, true};
    }
    if (agent->signed_in == false) {
        return {std::string(kWarning), "Sign in needed",
                command_text(agents::sign_in_hint(agent_id)), false, true};
    }
    std::vector<std::string> parts;
    for (const std::string* part : {&agent->version, &agent->account}) {
        if (!part->empty()) {
            parts.push_back(*part);
        }
    }
    return {std::string(kFound), "Found", join(parts, " · "), true, true};
}

std::string found_count_text(const agents::ScanResult& agents) {
    int count = 0;
    for (const std::string_view agent_id : agents::agent_order()) {
        if (found(agents, agent_id)) {
            ++count;
        }
    }
    if (count == 0) {
        return "No coding agents found on this computer. Pi can run AI insights "
               "with your API key once its component is installed.";
    }
    return std::format("Found {} coding {} on this computer.", count,
                       count == 1 ? "agent" : "agents");
}

std::string usage_caption(const agents::InstalledAgent& agent) {
    const std::string_view name = agent.spec().name;
    const std::string& account = agent.account;
    std::vector<std::string> lines;
    if (account.empty()) {
        // OpenCode reports its providers only once it is running.
        lines.push_back(std::format("Runs through the providers you signed in to in {}.", name));
        lines.emplace_back("Each meeting pass counts toward their usage.");
    } else {
        lines.push_back(std::format("Runs through your {} sign-in ({}).", name, account));
        if (account == "API key") {
            lines.emplace_back("Each meeting pass is billed to that key.");
        } else if (account.starts_with("Claude") || account == "ChatGPT") {
            lines.emplace_back("Each meeting pass counts toward that plan's usage.");
        } else {
            lines.emplace_back("Each meeting pass counts toward that account's usage.");
        }
    }
    lines.push_back(std::format("{} gets {}'s meeting tools and nothing else: no files, no shell.",
                                name, identity().product));
    return join(lines, " ");
}

std::string blocked_notice(std::string_view agent_id,
                           const std::optional<agents::InstalledAgent>& agent) {
    const std::string_view name = agents::agent_spec(agent_id).name;
    std::string_view until;
    std::string_view fix;
    if (!agent) {
        until = "it is installed";
        fix = "Install it";
    } else if (!agent->problem.empty()) {
        until = "it is updated";
        fix = "Update it";
    } else if (agent->signed_in == false) {
        until = "it is signed in";
        fix = "Sign in";
    } else {
        return "";
    }
    return std::format("AI insights can't start with {} until {}. {} and choose "
                       "Look again, or choose another option above.",
                       name, until, fix);
}

std::string purpose_count_text(const agents::ScanResult& agents, std::string_view purpose) {
    std::string text = found_count_text(agents);
    if (purpose == kCleanupPurpose) {
        bool any = false;
        for (const std::string_view agent_id : agents::agent_order()) {
            any = any || found(agents, agent_id);
        }
        if (!any) {
            text = "No coding agents found on this computer. Choose an API or local model below.";
        }
    }
    return text;
}

std::string purpose_blocked_notice(std::string_view agent_id,
                                   const std::optional<agents::InstalledAgent>& agent,
                                   std::string_view purpose) {
    return replace_all(blocked_notice(agent_id, agent), kInsightsPurpose, purpose);
}

std::string purpose_usage_caption(const agents::InstalledAgent& agent, std::string_view purpose) {
    std::string caption = usage_caption(agent);
    if (purpose == kCleanupPurpose) {
        caption = replace_all(std::move(caption), "meeting pass", "cleanup request");
        caption = replace_all(
            std::move(caption),
            std::format("gets {}'s meeting tools and nothing else: no files, no shell.",
                        identity().product),
            "receives the text and cleanup instructions, with no tools, files, or shell.");
        caption += " Starting an agent can take longer than an API request.";
    }
    return caption;
}

} // namespace fv::agent_picker
