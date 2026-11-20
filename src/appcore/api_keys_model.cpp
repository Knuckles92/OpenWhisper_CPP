#include "appcore/api_keys_model.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

#include "base/identity.h"

namespace fv::appcore {

namespace {

// str.join: the separator goes between items, empty ones included.
std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            joined += separator;
        }
        joined += parts[i];
    }
    return joined;
}

} // namespace

std::vector<CredentialEntry> key_entries(std::span<const CredentialOwner> owners) {
    std::vector<std::pair<std::string, std::vector<std::string>>> names;
    for (const auto& owner : owners) {
        auto row = std::ranges::find(names, owner.env_name,
                                     &std::pair<std::string, std::vector<std::string>>::first);
        if (row == names.end()) {
            names.emplace_back(owner.env_name, std::vector<std::string>{});
            row = std::prev(names.end());
        }
        if (std::ranges::find(row->second, owner.owner) == row->second.end()) {
            row->second.push_back(owner.owner);
        }
    }
    std::vector<CredentialEntry> entries;
    entries.reserve(names.size());
    for (const auto& [env_name, row_owners] : names) {
        entries.push_back({env_name, std::format("{} · {}", join(row_owners, " / "), env_name)});
    }
    return entries;
}

std::string api_keys_rail_value(std::span<const CredentialEntry> entries,
                                const std::function<CredentialSource(std::string_view)>& source) {
    if (entries.empty()) {
        return "None needed";
    }
    const auto available = std::ranges::count_if(entries, [&](const CredentialEntry& entry) {
        return source(entry.env_name) != CredentialSource::None;
    });
    return std::format("{} of {} set", available, entries.size());
}

KeyStatus status_text(CredentialSource source, std::string_view backend,
                      const std::optional<std::string>& key, std::string_view env_name,
                      bool shadowed) {
    KeyStatus status{"", "warning", ""};
    if (env_name.empty()) {
        status.text = "No endpoint needs an API key.";
    } else {
        const std::string suffix = mask_key(key.value_or(""));
        switch (source) {
        case CredentialSource::Store:
            status.text = std::format("Saved in {} · {}", backend, suffix);
            if (shadowed) {
                status.text += std::format(". The {} variable in your environment or .env file "
                                           "is ignored while a key is saved here.",
                                           env_name);
            }
            status.tone = "success";
            break;
        case CredentialSource::Environment:
            status.text = std::format("Using the {} environment variable · {}. Save a key here to "
                                      "use it instead.",
                                      env_name, suffix);
            status.tone = "success";
            break;
        case CredentialSource::DotEnv:
            status.text =
                std::format("Using the .env file · {}. Save a key here to use it instead.", suffix);
            status.tone = "success";
            break;
        case CredentialSource::None:
            status.text = "No key set.";
            break;
        }
    }
    status.icon = status.tone == "success" ? "check-green.svg" : "info-warning.svg";
    return status;
}

std::string uses_caption(std::string_view env_name, std::span<const CredentialOwner> owners) {
    if (env_name == kOpenAiKeyEnv) {
        return "Used for API transcription with OpenAI, OpenAI transcript cleanup and meeting "
               "intelligence, and cloud speaker identification.";
    }
    if (env_name == kOpenRouterKeyEnv) {
        return "Used for transcript cleanup and meeting intelligence through OpenRouter. It can "
               "also run fast judgments (Jev): choose OpenRouter under Meeting Mode → Fast "
               "judgments.";
    }
    if (env_name == kTypeSafeKeyEnv) {
        return "Used for TypeSafe fast judgments: topic changes, spoken instructions, live "
               "highlights, the questions radar, citation checks, semantic search, and insight "
               "review. Enable them under Meeting Mode → Fast judgments, where an OpenRouter key "
               "can run the same model instead.";
    }
    if (env_name.empty()) {
        return "";
    }
    std::vector<std::string> text_owners;
    std::vector<std::string> speech_owners;
    for (const auto& owner : owners) {
        if (owner.env_name != env_name) {
            continue;
        }
        if (owner.use == kUseText) {
            text_owners.push_back(owner.owner);
        } else if (owner.use == kUseSpeech) {
            speech_owners.push_back(owner.owner);
        }
    }
    std::vector<std::string> uses;
    if (!text_owners.empty()) {
        uses.push_back(std::format("to {} for transcript cleanup and meeting intelligence",
                                   join(text_owners, " and ")));
    }
    if (!speech_owners.empty()) {
        uses.push_back(std::format("to {} for API transcription", join(speech_owners, " and ")));
    }
    return std::format("Sent as the API key {}.", join(uses, ", and "));
}

std::string store_caption(const CredentialStatus& status, bool mac) {
    if (!status.available) {
        return std::format("{} isn't available on this computer, so keys can't be saved from "
                           "here. Set the variable in your environment or a .env file instead; "
                           "{} never falls back to an unprotected file.",
                           status.backend_name, identity().product);
    }
    std::string copy = std::format("Encrypted by {} for your account. Saved keys override "
                                   "environment variables and .env, and never enter settings or "
                                   "logs.",
                                   status.backend_name);
    if (mac) {
        copy += " When macOS asks to use your Keychain, choose Always Allow so it doesn't ask "
                "every launch.";
    }
    return copy;
}

std::string store_detail(const CredentialStatus& status) {
    if (status.available || status.reason.empty()) {
        return "";
    }
    return std::format("Details: {}", status.reason);
}

} // namespace fv::appcore
