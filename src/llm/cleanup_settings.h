#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "llm/profiles.h"

// services/settings.py's text-model resolvers (1664-1748, 1922-1973): which endpoint and model
// AI cleanup and meeting intelligence use, from a settings snapshot. A provider/model pair is kept
// only while the provider is still a known profile and the model is not blank; otherwise both
// fall back to OpenRouter's defaults. Pure and thread-safe.
namespace fv::llm {

// config.py's cleanup defaults.
inline constexpr std::string_view kTranscriptCleanupProvider = "openrouter";
inline constexpr std::string_view kTranscriptCleanupModel = "gpt-6-luna";
inline constexpr std::string_view kTranscriptCleanupOpenRouterModel = "openrouter/free";
inline constexpr bool kTranscriptCleanupEnabled = false;
inline constexpr double kTranscriptCleanupTimeoutS = 8.0;

// TranscriptCleanupProvider.ALL: the built-in endpoint ids, in order.
inline constexpr std::array<std::string_view, 10> kTranscriptCleanupProviders{
    "openai",    "openrouter", "ollama", "lm_studio",   "vllm",
    "llama_cpp", "localai",    "groq",   "opencode_go", "opencode_zen"};
// Coding agents the user can select for dictation cleanup.
inline constexpr std::array<std::string_view, 3> kInstalledCleanupAgents{"claude_code", "codex",
                                                                         "opencode_cli"};
bool is_installed_cleanup_agent(std::string_view provider);
// Checked against current settings immediately before the transcript leaves the app.
bool cleanup_agent_authorized(const Json& settings, std::string_view provider);

// The ids of every profile (built-in, then custom).
std::vector<std::string> known_text_llm_profile_ids(const Json& settings);

// A provider's default cleanup model: "" for an installed agent or a custom endpoint, OpenRouter's
// "openrouter/free", OpenAI's "gpt-6-luna", and "gpt-6-luna" for an id that names no profile.
// Python looks the profile up in the saved settings; `settings` is that snapshot.
std::string default_transcript_cleanup_model(std::string_view provider, const Json& settings);

// A known cleanup profile id (or a saved installed-agent id with a saved model, even blank), else
// "openrouter".
std::string resolve_transcript_cleanup_provider(const Json& settings);
// Its model, stripped, else "openrouter/free". OpenAI's retired "gpt-4o-mini" reads as
// "gpt-6-luna" without the settings being written.
std::string resolve_transcript_cleanup_model(const Json& settings);
// One of the thinking levels (off .. ultra), else "off".
std::string resolve_transcript_cleanup_reasoning(const Json& settings);
// One of the catalog sorts, else "alphabetical".
std::string resolve_transcript_cleanup_model_sort(const Json& settings);

// Meeting intelligence: the same pair rule over meeting_llm_provider and meeting_llm_model.
std::string resolve_meeting_llm_provider(const Json& settings);
TextLLMProfile resolve_meeting_llm_profile(const Json& settings);
std::string resolve_meeting_llm_model(const Json& settings);
// The non-secret endpoint snapshot (to_dict) of the meeting profile and model, from what is
// known now; OpenWhisper first asks a server the user runs about its models, which the overload
// in llm/local_servers.h does.
Json resolve_meeting_llm_endpoint(const Json& settings);

} // namespace fv::llm
