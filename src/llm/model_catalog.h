#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// services/text_model_catalog.py: what a text model is (its API protocol, limits and reasoning
// control), from the reviewed OpenCode route table, OpenAI's id rules, saved meeting metadata,
// and what servers the user runs reported about their models.
namespace fv::llm {

struct TextLLMProfile;

struct TextModelSpec {
    std::string protocol = "chat"; // "chat", "responses", "anthropic", "google"
    bool tools = true;
    std::int64_t context_window = 32768;   // tokens
    std::int64_t max_output_tokens = 4096; // tokens
    bool reasoning = false;
    std::string reasoning_format;  // "", "openai", "deepseek", "qwen"
    std::optional<Json> pi_compat; // a JSON object
    // Cleanup level -> the value the server takes, or null where the model cannot take it.
    std::optional<Json> thinking_levels;

    // dataclasses.asdict: every field, in declaration order, None for an unset dict.
    Json to_dict() const;
    // Python ==: the dicts compare regardless of key order.
    bool operator==(const TextModelSpec& other) const;
};

// What one server URL reported, replacing what it reported before: {model: spec}. The cache is
// process-wide, keyed by the base URL exactly as given, and never invalidated except by the next
// discovery. Thread-safe.
void remember_server_specs(std::string_view base_url, std::map<std::string, TextModelSpec> specs);
bool server_specs_known(std::string_view base_url);
// Forgets every server (tests only: OpenWhisper's conftest resets the cache between tests).
void forget_server_specs();

// The spec of `model` on `profile`. OpenAI's routes are recomputed every time; otherwise saved
// metadata for the same model wins, then a custom Responses endpoint, then what a server the user
// runs reported (a modest default when it said nothing), then the OpenCode route table, Groq,
// and the large hosted defaults. Throws std::invalid_argument with the user's reason: "Choose a
// text model first.", "<model> has no supported API route for <name>. Choose a supported
// model.", "Choose a Groq text-generation model.", or spec_from_mapping's errors.
TextModelSpec model_spec(const TextLLMProfile& profile, std::string_view model);

// A saved spec (meeting metadata), with context clamped to [1024, 2000000] and output to
// [16, 131072]. Throws std::invalid_argument("Unsupported saved text protocol.") for a protocol
// outside chat, responses, anthropic and google; an unconvertible limit throws as Python's
// int() does (py::TypeError, py::ValueError, py::OverflowError).
TextModelSpec spec_from_mapping(const Json& raw);

// The route table: kind -> protocol -> model ids, in the catalog's order (opencode_go,
// opencode_zen). Empty for any other kind.
const Json& catalog_routes();
// kind -> model -> spec fields.
const Json& catalog_capabilities();
// Every model the route table names for `kind`, first route first, without repeats.
std::vector<std::string> catalog_models(std::string_view kind);

} // namespace fv::llm
