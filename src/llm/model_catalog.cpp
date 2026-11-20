#include "llm/model_catalog.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "llm/data/text_model_catalog.json.h"
#include "llm/detail/truthy.h"
#include "llm/profiles.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::llm {

namespace {

const Json& catalog() {
    static const Json document = Json::parse(embedded::llm_text_model_catalog);
    return document;
}

std::vector<std::string> strings(std::string_view name) {
    std::vector<std::string> out;
    for (const auto& item : catalog().at(std::string(name))) {
        out.push_back(item.get<std::string>());
    }
    return out;
}

const std::vector<std::string>& openai_reasoning_prefixes() {
    static const auto list = strings("openai_reasoning_prefixes");
    return list;
}
const std::vector<std::string>& openai_responses_text_prefixes() {
    static const auto list = strings("openai_responses_text_prefixes");
    return list;
}
const std::vector<std::string>& openai_non_text_markers() {
    static const auto list = strings("openai_non_text_markers");
    return list;
}
const std::vector<std::string>& groq_non_text_markers() {
    static const auto list = strings("groq_non_text_markers");
    return list;
}
const std::vector<std::string>& saved_protocols() {
    static const auto list = strings("saved_protocols");
    return list;
}
const std::vector<std::string>& saved_reasoning_formats() {
    static const auto list = strings("saved_reasoning_formats");
    return list;
}
std::int64_t large_context_window() {
    return catalog().at("large_context_window").get<std::int64_t>();
}
std::int64_t large_max_output_tokens() {
    return catalog().at("large_max_output_tokens").get<std::int64_t>();
}

bool starts_with_any(std::string_view text, const std::vector<std::string>& prefixes) {
    return std::ranges::any_of(prefixes, [&](const std::string& p) { return text.starts_with(p); });
}

bool contains_any(std::string_view text, const std::vector<std::string>& markers) {
    return std::ranges::any_of(
        markers, [&](const std::string& m) { return text.find(m) != std::string_view::npos; });
}

std::string without_ft_prefix(std::string lowered) {
    if (lowered.starts_with("ft:")) {
        lowered.erase(0, 3);
    }
    return lowered;
}

std::mutex& server_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::map<std::string, TextModelSpec>>& server_specs() {
    static std::unordered_map<std::string, std::map<std::string, TextModelSpec>> specs;
    return specs;
}

TextModelSpec large_spec() {
    TextModelSpec spec;
    spec.context_window = large_context_window();
    spec.max_output_tokens = large_max_output_tokens();
    return spec;
}

TextModelSpec openai_model_spec(std::string_view model) {
    // Fine-tuned ids keep their base model after the ft: prefix.
    std::string base = without_ft_prefix(py::lower(model));
    if (const auto colon = base.find(':'); colon != std::string::npos) {
        base.resize(colon);
    }
    const bool reasoning = starts_with_any(base, openai_reasoning_prefixes());
    const bool responses = reasoning || (starts_with_any(base, openai_responses_text_prefixes()) &&
                                         !contains_any(base, openai_non_text_markers()));
    TextModelSpec spec = large_spec();
    spec.protocol = responses ? "responses" : "chat";
    spec.reasoning = reasoning;
    spec.reasoning_format = reasoning ? "openai" : "";
    if (base == "gpt-6-luna") {
        // Omitting effort uses Luna's medium default, even when the UI says Off.
        spec.thinking_levels = Json{{"off", "none"}};
    }
    return spec;
}

// TextModelSpec(protocol=protocol, **capabilities): the catalog's fields as they are.
TextModelSpec routed_spec(std::string_view protocol, const Json& capabilities) {
    TextModelSpec spec;
    spec.protocol = std::string(protocol);
    if (const Json& value = detail::get(capabilities, "tools"); !value.is_null()) {
        spec.tools = value.get<bool>();
    }
    if (const Json& value = detail::get(capabilities, "context_window"); !value.is_null()) {
        spec.context_window = value.get<std::int64_t>();
    }
    if (const Json& value = detail::get(capabilities, "max_output_tokens"); !value.is_null()) {
        spec.max_output_tokens = value.get<std::int64_t>();
    }
    if (const Json& value = detail::get(capabilities, "reasoning"); !value.is_null()) {
        spec.reasoning = value.get<bool>();
    }
    if (const Json& value = detail::get(capabilities, "reasoning_format"); !value.is_null()) {
        spec.reasoning_format = value.get<std::string>();
    }
    if (detail::has(capabilities, "pi_compat")) {
        spec.pi_compat = capabilities.at("pi_compat");
    }
    if (detail::has(capabilities, "thinking_levels")) {
        spec.thinking_levels = capabilities.at("thinking_levels");
    }
    return spec;
}

TextModelSpec routed_model_spec(const TextLLMProfile& profile, std::string_view model) {
    for (const auto& [protocol, models] : catalog_routes().at(profile.kind).items()) {
        for (const auto& candidate : models) {
            if (candidate.get_ref<const std::string&>() == model) {
                const Json& capabilities =
                    detail::get(detail::get(catalog_capabilities(), profile.kind), model);
                return routed_spec(protocol, capabilities);
            }
        }
    }
    throw std::invalid_argument(std::string(model) + " has no supported API route for " +
                                profile.name + ". Choose a supported model.");
}

// _clamped: max(low, min(high, int(value))), with int() as Python runs it on a JSON value.
std::int64_t clamped(const Json& value, std::int64_t low, std::int64_t high) {
    const auto converted = py::py_int_checked(value);
    if (converted) {
        return std::max(low, std::min(high, *converted));
    }
    switch (converted.error()) {
    case py::IntError::Type:
        throw py::TypeError("int() argument must be a string, a bytes-like object or a real "
                            "number, not '" +
                            std::string(value.is_array()    ? "list"
                                        : value.is_object() ? "dict"
                                                            : "NoneType") +
                            "'");
    case py::IntError::Value:
        if (value.is_number_float()) {
            throw py::ValueError("cannot convert float NaN to integer");
        }
        throw py::ValueError("invalid literal for int() with base 10: " +
                             py::repr(value.is_string() ? value.get<std::string>() : ""));
    case py::IntError::Overflow:
        if (value.is_number_float() && std::isinf(value.get<double>())) {
            throw py::OverflowError("cannot convert float infinity to integer");
        }
        // Python holds the integer and clamps it; only its sign matters here.
        if (value.is_number_float()) {
            return value.get<double>() < 0 ? low : high;
        }
        if (const auto* text = detail::text(value)) {
            return py::strip(*text).starts_with('-') ? low : high;
        }
        return high;
    }
    return low;
}

bool contains(const std::vector<std::string>& list, const Json& value) {
    const auto* text = detail::text(value);
    return text != nullptr && std::ranges::find(list, *text) != list.end();
}

} // namespace

Json TextModelSpec::to_dict() const {
    Json out = Json::object();
    out["protocol"] = protocol;
    out["tools"] = tools;
    out["context_window"] = context_window;
    out["max_output_tokens"] = max_output_tokens;
    out["reasoning"] = reasoning;
    out["reasoning_format"] = reasoning_format;
    out["pi_compat"] = pi_compat ? *pi_compat : Json(nullptr);
    out["thinking_levels"] = thinking_levels ? *thinking_levels : Json(nullptr);
    return out;
}

bool TextModelSpec::operator==(const TextModelSpec& other) const {
    auto same = [](const std::optional<Json>& a, const std::optional<Json>& b) {
        return a.has_value() == b.has_value() && (!a || py::py_equal(*a, *b));
    };
    return protocol == other.protocol && tools == other.tools &&
           context_window == other.context_window && max_output_tokens == other.max_output_tokens &&
           reasoning == other.reasoning && reasoning_format == other.reasoning_format &&
           same(pi_compat, other.pi_compat) && same(thinking_levels, other.thinking_levels);
}

void remember_server_specs(std::string_view base_url, std::map<std::string, TextModelSpec> specs) {
    std::lock_guard lock(server_mutex());
    server_specs()[std::string(base_url)] = std::move(specs);
}

bool server_specs_known(std::string_view base_url) {
    std::lock_guard lock(server_mutex());
    return server_specs().contains(std::string(base_url));
}

void forget_server_specs() {
    std::lock_guard lock(server_mutex());
    server_specs().clear();
}

TextModelSpec model_spec(const TextLLMProfile& profile, std::string_view model) {
    if (model.empty() || py::strip(model).empty()) {
        throw std::invalid_argument("Choose a text model first.");
    }
    // OpenAI's supported protocols change independently of a meeting snapshot; recalculating
    // them keeps a retry from reusing an obsolete chat route.
    if (profile.kind == "openai") {
        return openai_model_spec(model);
    }
    if (profile.model_metadata && detail::truthy(*profile.model_metadata)) {
        const auto* saved = detail::text(detail::get(*profile.model_metadata, "model"));
        if (saved != nullptr && *saved == model) {
            return spec_from_mapping(*profile.model_metadata);
        }
    }
    if (profile.kind == "custom" && profile.protocol == "responses") {
        const bool reasoning =
            starts_with_any(without_ft_prefix(py::lower(model)), openai_reasoning_prefixes());
        TextModelSpec spec = large_spec();
        spec.protocol = "responses";
        spec.reasoning = reasoning;
        spec.reasoning_format = reasoning ? "openai" : "";
        return spec;
    }
    if (profile.self_hosted()) {
        // What the server reported, else a modest default rather than a hosted model's limits.
        std::lock_guard lock(server_mutex());
        const auto server = server_specs().find(profile.base_url.value_or(""));
        if (server != server_specs().end()) {
            if (const auto found = server->second.find(std::string(model));
                found != server->second.end()) {
                return found->second;
            }
        }
        return TextModelSpec{};
    }
    if (detail::has(catalog_routes(), profile.kind)) {
        return routed_model_spec(profile, model);
    }
    if (profile.kind == "groq") {
        if (contains_any(py::lower(model), groq_non_text_markers())) {
            throw std::invalid_argument("Choose a Groq text-generation model.");
        }
        return TextModelSpec{};
    }
    TextModelSpec spec = large_spec();
    spec.reasoning = profile.kind == "openai" || profile.kind == "openrouter";
    return spec;
}

TextModelSpec spec_from_mapping(const Json& raw) {
    const Json protocol = detail::has(raw, "protocol") ? raw.at("protocol") : Json("chat");
    if (!contains(saved_protocols(), protocol)) {
        throw std::invalid_argument("Unsupported saved text protocol.");
    }
    const Json reasoning_format =
        detail::has(raw, "reasoning_format") ? raw.at("reasoning_format") : Json("");
    const Json& pi_compat = detail::get(raw, "pi_compat");
    const Json& thinking_levels = detail::get(raw, "thinking_levels");
    TextModelSpec spec;
    spec.protocol = protocol.get<std::string>();
    spec.tools = !detail::has(raw, "tools") || raw.at("tools") == Json(true);
    spec.context_window = clamped(
        detail::has(raw, "context_window") ? raw.at("context_window") : Json(32768), 1024, 2000000);
    spec.max_output_tokens =
        clamped(detail::has(raw, "max_output_tokens") ? raw.at("max_output_tokens") : Json(4096),
                16, 131072);
    spec.reasoning = detail::get(raw, "reasoning") == Json(true);
    spec.reasoning_format = contains(saved_reasoning_formats(), reasoning_format)
                                ? reasoning_format.get<std::string>()
                                : std::string();
    if (pi_compat.is_object()) {
        spec.pi_compat = pi_compat;
    }
    if (thinking_levels.is_object()) {
        spec.thinking_levels = thinking_levels;
    }
    return spec;
}

const Json& catalog_routes() {
    return catalog().at("routes");
}

const Json& catalog_capabilities() {
    return catalog().at("capabilities");
}

std::vector<std::string> catalog_models(std::string_view kind) {
    std::vector<std::string> models;
    const Json& routes = detail::get(catalog_routes(), kind);
    if (!routes.is_object()) {
        return models;
    }
    for (const auto& [protocol, list] : routes.items()) {
        for (const auto& model : list) {
            auto name = model.get<std::string>();
            if (std::ranges::find(models, name) == models.end()) {
                models.push_back(std::move(name));
            }
        }
    }
    return models;
}

} // namespace fv::llm
