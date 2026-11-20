#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The bundled profile of each downloadable speech runtime, shown by the component rows and their
// details dialog without contacting the network (services/component_catalog.py: ComponentDetails,
// _SOURCE_NOTE, _SPEECH_RUNTIME_SOURCE_NOTE and the NVIDIA rows). Install URLs, sizes and digests
// stay in catalog.h; this is copy and links only.
//
// Where FioriVoce differs: the runtime is a set of native libraries loaded into the app's own
// process, so the rows say so instead of "Runs in a dedicated process" and "Isolated worker
// process", carry no portable Python (payload, licence and the Python download link go), and
// name only Parakeet among the engines that share them. The CPU and CUDA rows are offered on
// Windows; the Vulkan and Metal rows are kept for the platforms that offer them.
// Affinity: thread-safe (immutable after first use).
namespace fv::models {

struct ComponentDetails {
    std::string component_id;
    std::string display_name;
    std::string summary;
    std::string description;
    std::string origin_name;
    std::string origin_url;
    std::string origin_label;
    std::string source_name;
    std::string source_url;
    std::string source_label;
    std::string maintainer;
    std::string family;
    std::string requires_text; // ComponentDetails.requires (a C++ keyword)
    std::string payload;
    std::string local_format;
    std::string license;
    std::string best_for;
    std::vector<std::string> limitations;
    std::string compact_tags;
    std::string source_note;
    std::vector<std::string> source_urls;
};

// _SOURCE_NOTE: what the pins and the atomic swap promise.
std::string_view component_source_note();
// _SPEECH_RUNTIME_SOURCE_NOTE: _SOURCE_NOTE plus the third-party executable warning.
std::string speech_runtime_source_note();

// Every profile, in runtime_ids() order.
std::span<const ComponentDetails> component_catalog();
// get_component_details: throws std::out_of_range (Python's KeyError) for an unknown id.
const ComponentDetails& get_component_details(std::string_view component_id);
// nullptr where get_component_details throws.
const ComponentDetails* find_component_details(std::string_view component_id);

// (display name, summary) for ComponentCoordinator::Options::copy_for: the id and "" for a
// component without a profile, as the coordinator does without copy.
std::pair<std::string, std::string> component_copy(std::string_view component_id);

} // namespace fv::models
