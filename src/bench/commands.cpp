#include "bench/commands.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <set>

#include "base/utf8.h"
#include "bench/ami.h"
#include "bench/local_asr.h"
#include "bench/meeting_metrics.h"
#include "bench/provenance.h"
#include "llm/cleanup_settings.h"
#include "meeting_capture/offline.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "speech/file_windows.h"

namespace fv::bench {

namespace {

constexpr std::string_view kUsage =
    "usage: openwhisper-cpp-bench {live-preview,live-preview-report,cleanup-levels,meeting,asr,"
    "asr-corpus} ...";

// A command line split as argparse reads it: --name value, --name=value, flags and positionals.
class Arguments {
public:
    Arguments(const std::vector<std::string>& args, std::set<std::string> flags,
              std::set<std::string> options)
        : flags_(std::move(flags)), options_(std::move(options)) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (!arg.starts_with("--") || arg == "--") {
                positionals_.push_back(arg);
                continue;
            }
            const auto equals = arg.find('=');
            const std::string name = arg.substr(0, equals);
            if (flags_.contains(name)) {
                if (equals != std::string::npos) {
                    throw UsageError("argument " + name + ": ignored explicit argument");
                }
                values_[name].emplace_back();
                continue;
            }
            if (!options_.contains(name)) {
                throw UsageError("unrecognized arguments: " + arg);
            }
            if (equals != std::string::npos) {
                values_[name].push_back(arg.substr(equals + 1));
            } else if (i + 1 < args.size()) {
                values_[name].push_back(args[++i]);
            } else {
                throw UsageError("argument " + name + ": expected one argument");
            }
        }
    }

    bool flag(const std::string& name) const { return values_.contains(name); }
    std::optional<std::string> last(const std::string& name) const {
        const auto found = values_.find(name);
        return found == values_.end() ? std::nullopt : std::optional(found->second.back());
    }
    std::vector<std::string> all(const std::string& name) const {
        const auto found = values_.find(name);
        return found == values_.end() ? std::vector<std::string>{} : found->second;
    }
    const std::vector<std::string>& positionals() const { return positionals_; }

    double number(const std::string& name, double fallback) const {
        const auto text = last(name);
        if (!text) {
            return fallback;
        }
        // float(): decimal, exponent, inf and nan forms.
        const std::string stripped = py::strip(*text);
        char* end = nullptr;
        const double value = std::strtod(stripped.c_str(), &end);
        if (stripped.empty() || end != stripped.c_str() + stripped.size()) {
            throw UsageError("argument " + name + ": invalid float value: " + py::repr(*text));
        }
        return value;
    }
    std::optional<int> integer(const std::string& name) const {
        const auto text = last(name);
        if (!text) {
            return std::nullopt;
        }
        const std::string stripped = py::strip(*text);
        int value = 0;
        const auto [end, error] =
            std::from_chars(stripped.data(), stripped.data() + stripped.size(), value);
        if (stripped.empty() || error != std::errc() || end != stripped.data() + stripped.size()) {
            throw UsageError("argument " + name + ": invalid int value: " + py::repr(*text));
        }
        return value;
    }

private:
    std::set<std::string> flags_;
    std::set<std::string> options_;
    std::map<std::string, std::vector<std::string>> values_;
    std::vector<std::string> positionals_;
};

const std::set<std::string> kEngineOptions = {"--runtime", "--model", "--device"};

std::set<std::string> with_engine(std::set<std::string> options) {
    options.insert(kEngineOptions.begin(), kEngineOptions.end());
    return options;
}

std::string device_of(const Arguments& args) {
    const std::string device = args.last("--device").value_or("cpu");
    if (device != "cpu" && device != "cuda") {
        throw UsageError("argument --device: invalid choice: " + py::repr(device) +
                         " (choose from 'cpu', 'cuda')");
    }
    return device;
}

std::unique_ptr<BenchEngine> load(CommandContext& context, const EngineFiles& files) {
    return context.load_engine ? context.load_engine(files) : load_parakeet(files);
}

std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> out;
    for (const std::string& item : py::split_on(text, ",")) {
        out.push_back(item);
    }
    return out;
}

std::string left(std::string text, std::size_t width) {
    const auto length = static_cast<std::size_t>(py::len(text));
    if (length < width) {
        text.append(width - length, ' ');
    }
    return text;
}

std::string right(std::string text, std::size_t width) {
    const auto length = static_cast<std::size_t>(py::len(text));
    if (length < width) {
        text.insert(0, width - length, ' ');
    }
    return text;
}

std::string percent_or_na(const Json& value) {
    return value.is_null() ? std::string("n/a")
                           : py::format_fixed(value.get<double>() * 100.0, 1) + "%";
}

// --- live-preview ------------------------------------------------------------------------------

int live_preview(const std::vector<std::string>& argv, CommandContext& context) {
    const LivePreviewArgs args = parse_live_preview_args(argv);
    const EngineLookup lookup =
        find_engine_files(args.runtime, args.model, context.env, args.device);
    if (!lookup.files) {
        context.err << lookup.missing << '\n';
        return 2;
    }
    auto [manifest, clips] = load_clips(args.manifest, args.limit);
    context.out << "Loading Parakeet on " << args.device << std::endl;
    auto engine = load(context, *lookup.files);
    Json metrics;
    bool failed = false;
    try {
        metrics =
            run_live_preview(engine->service(), manifest, clips, args,
                             [&](const std::string& line) { context.out << line << std::endl; });
    } catch (const std::exception& error) {
        metrics = Json{{"error", error.what()}};
        failed = true;
    }
    metrics["load_s"] = engine->load_s();
    metrics["device_info"] = engine->readiness().device_info;
    const Json report =
        make_report("live-preview", metrics,
                    report_provenance(context.source_root, lookup.files->runtime_dir,
                                      lookup.files->model_path));
    write_json(args.output, report);
    if (failed) {
        context.err << metrics["error"].get<std::string>() << '\n';
        return 1;
    }
    const Json& summary = metrics["summary"];
    context.out << "Update p50 / p95 (ms): "
                << (summary["update_median_ms"].is_null()
                        ? std::string("none")
                        : py::format_fixed(summary["update_median_ms"].get<double>(), 0))
                << " / "
                << (summary["update_p95_ms"].is_null()
                        ? std::string("none")
                        : py::format_fixed(summary["update_p95_ms"].get<double>(), 0))
                << ", live WER " << percent_or_na(summary["live"]["wer"]) << ", drained WER "
                << percent_or_na(summary["drained"]["wer"]) << ", RTF "
                << py::format_fixed(summary["rtf"].get<double>(), 3) << '\n'
                << "Wrote " << path_to_utf8(args.output) << '\n';
    return 0;
}

int live_preview_report(const std::vector<std::string>& argv, CommandContext& context) {
    const Arguments args(argv, {}, {"--output"});
    if (args.positionals().empty()) {
        throw UsageError("the following arguments are required: inputs");
    }
    const auto output = args.last("--output");
    if (!output) {
        throw UsageError("the following arguments are required: --output");
    }
    std::vector<Json> reports;
    std::vector<std::string> names;
    for (const std::string& input : args.positionals()) {
        const auto path = path_from_utf8(input);
        reports.push_back(read_json(path));
        names.push_back(path_to_utf8(path.filename()));
    }
    const Json pooled = pool_live_preview_reports(reports, names);
    auto base = path_from_utf8(*output);
    write_json(std::filesystem::path(base).replace_extension(".json"), pooled);
    const std::string markdown = live_preview_markdown(pooled);
    std::ofstream(std::filesystem::path(base).replace_extension(".md"), std::ios::binary)
        << markdown;
    context.out << markdown;
    return 0;
}

// --- cleanup-levels ----------------------------------------------------------------------------

int cleanup_levels(const std::vector<std::string>& argv, CommandContext& context) {
    const Arguments args(
        argv, {},
        {"--provider", "--model", "--reasoning", "--level", "--case", "--delay", "--json"});
    if (!args.positionals().empty()) {
        throw UsageError("unrecognized arguments: " + args.positionals().front());
    }
    const double delay = args.number("--delay", 0.0);
    std::vector<Json> cases = load_cases();
    if (const auto levels = args.all("--level"); !levels.empty()) {
        std::erase_if(cases, [&](const Json& item) {
            return std::find(levels.begin(), levels.end(), case_label(item)) == levels.end();
        });
    }
    if (const auto ids = args.all("--case"); !ids.empty()) {
        std::erase_if(cases, [&](const Json& item) {
            return std::find(ids.begin(), ids.end(), item.at("id").get<std::string>()) == ids.end();
        });
    }
    if (cases.empty()) {
        context.out << "No cases match.\n";
        return 2;
    }
    const Json settings = context.load_settings ? context.load_settings() : Json::object();
    const std::string provider =
        args.last("--provider").value_or(llm::resolve_transcript_cleanup_provider(settings));
    const std::string model =
        args.last("--model").value_or(llm::resolve_transcript_cleanup_model(settings));
    const std::string reasoning =
        args.last("--reasoning").value_or(llm::resolve_transcript_cleanup_reasoning(settings));
    if (!context.make_cleaner) {
        context.out << "AI cleanup is not set up for " << provider << ": no API key or model.\n";
        return 2;
    }
    const CleanupClientHandle cleaner = context.make_cleaner(provider, model, reasoning);
    if (!cleaner.is_available || !cleaner.is_available()) {
        context.out << "AI cleanup is not set up for " << provider << ": no API key or model.\n";
        return 2;
    }
    context.out << cases.size() << " cases \u00B7 " << cleaner.provider << " \u00B7 "
                << cleaner.model << " \u00B7 thinking " << cleaner.reasoning << std::endl;
    const auto results = run(cases, cleaner.cleaner, delay);
    for (const CaseResult& result : results) {
        context.out << (result.passed ? "pass" : "FAIL") << "  " << left(result.label, 22) << " "
                    << left(result.id, 28) << " " << right(py::format_fixed(result.elapsed_s, 1), 5)
                    << "s\n";
        if (!result.passed) {
            std::string failures;
            for (const std::string& failure : result.failures) {
                failures += (failures.empty() ? "" : "; ") + failure;
            }
            std::string output = result.output;
            for (auto at = output.find('\n'); at != std::string::npos;
                 at = output.find('\n', at + 7)) {
                output.replace(at, 1, "\n      ");
            }
            context.out << "      " << failures << "\n      " << output << '\n';
        }
    }
    const CleanupSummary summary = summarize(results);
    context.out << "\nAll: " << rate(summary.passed, summary.total) << '\n';
    for (const auto* group : {&summary.by_label, &summary.by_category}) {
        for (const auto& [key, counts] : *group) {
            context.out << "  " << left(key, 24) << " " << rate(counts.first, counts.second)
                        << '\n';
        }
    }
    if (const auto json = args.last("--json")) {
        Json rows = Json::array();
        for (const CaseResult& result : results) {
            rows.push_back(result.to_json());
        }
        write_json(path_from_utf8(*json), Json{{"provider", cleaner.provider},
                                               {"model", cleaner.model},
                                               {"summary", summary.to_json()},
                                               {"results", rows}});
    }
    return 0;
}

// --- meeting -----------------------------------------------------------------------------------

int meeting(const std::vector<std::string>& argv, CommandContext& context) {
    const MeetingArgs args = parse_meeting_args(argv);
    auto meetings = select_meetings(args.meetings);
    if (args.max_meetings) {
        meetings.resize(
            std::min(meetings.size(), static_cast<std::size_t>(std::max(0, *args.max_meetings))));
    }
    if (meetings.empty()) {
        context.err << "No meetings selected\n";
        return 2;
    }
    const auto annotations = annotation_root(args.data_dir);
    std::vector<std::string> missing;
    for (const MeetingSpec& spec : meetings) {
        if (!std::filesystem::exists(audio_path(args.data_dir, spec.meeting_id))) {
            missing.push_back(spec.meeting_id);
        }
    }
    if (!missing.empty()) {
        std::string names;
        for (const std::string& id : missing) {
            names += (names.empty() ? "" : ", ") + id;
        }
        context.err << "Missing AMI audio for " << names << ". Put the headset mixes in "
                    << path_to_utf8(args.data_dir / "audio") << " and the extracted annotations in "
                    << path_to_utf8(annotations) << ": openwhisper-cpp-bench never downloads.\n";
        return 2;
    }
    if (args.target_sec <= 0 || args.max_sec < args.target_sec) {
        context.err << "Chunk seconds must satisfy 0 < target <= maximum\n";
        return 2;
    }
    const EngineLookup lookup =
        find_engine_files(args.runtime, args.model, context.env, args.device);
    if (!lookup.files) {
        context.err << lookup.missing << '\n';
        return 2;
    }
    const std::string language = py::lower(py::strip(args.language));
    const std::string run_name = "parakeet-" + language + "-t" + py::float_repr(args.target_sec) +
                                 "-m" + py::float_repr(args.max_sec) +
                                 (args.offline_pass ? "-offline" : "");
    const auto run_dir = args.results_dir / path_from_utf8(run_name);
    std::filesystem::create_directories(run_dir);

    auto engine = load(context, *lookup.files);
    speech::SpeechService& service = engine->service();
    MeetingDecoders decoders;
    decoders.draft = [&service, language](std::vector<float> audio) {
        speech::DecodeJob job;
        speech::AudioClip clip;
        clip.end = audio.size();
        clip.samples = std::make_shared<const std::vector<float>>(std::move(audio));
        job.input = std::move(clip);
        job.options.language = language;
        job.lane = speech::Lane::Meeting;
        job.slot = speech::Slot::Meeting;
        return service.run(std::move(job)).transcript.segments;
    };
    if (args.offline_pass) {
        decoders.offline =
            meeting::background_window_decoder(service, language, {}, speech::Slot::Meeting);
    }
    const Json model_stamp{{"runtime", path_to_utf8(lookup.files->runtime_dir)},
                           {"model", path_to_utf8(lookup.files->model_path)},
                           {"device_info", engine->readiness().device_info}};

    std::vector<Json> results;
    for (std::size_t index = 0; index < meetings.size(); ++index) {
        const MeetingSpec& spec = meetings[index];
        const auto source = audio_path(args.data_dir, spec.meeting_id);
        const auto result_path = run_dir / path_from_utf8(spec.meeting_id + ".json");
        IdentityRequest request;
        request.source_root = context.source_root;
        request.inputs.push_back(source);
        std::vector<std::filesystem::path> xml;
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(annotations, error);
             !error && it != std::filesystem::recursive_directory_iterator(); ++it) {
            const std::string name = path_to_utf8(it->path().filename());
            if (name.starts_with(spec.meeting_id) && name.ends_with(".xml")) {
                xml.push_back(it->path());
            }
        }
        std::sort(xml.begin(), xml.end());
        request.inputs.insert(request.inputs.end(), xml.begin(), xml.end());
        request.settings = Json{{"model", "parakeet-v3"},
                                {"language", language},
                                {"target_sec", args.target_sec},
                                {"max_sec", args.max_sec},
                                {"offline", args.offline_pass}};
        request.model = model_stamp;
        const Json provenance = identity(request);
        const std::string counter =
            "[" + std::to_string(index + 1) + "/" + std::to_string(meetings.size()) + "] ";
        if (std::filesystem::exists(result_path) && !args.force) {
            const Json cached = read_json(result_path);
            if (reusable(cached, provenance)) {
                context.out << counter << "Reusing verified " << path_to_utf8(result_path)
                            << std::endl;
                results.push_back(cached);
                continue;
            }
            context.out << "  Cached provenance differs; decoding " << spec.meeting_id << " again"
                        << std::endl;
        }
        context.out << counter << "Decoding " << spec.meeting_id << ": " << spec.description
                    << std::endl;
        Json result =
            decode_meeting(decoders, spec, source, args.target_sec, args.max_sec, args.offline_pass,
                           [&](const std::string& line) { context.out << line << std::endl; });
        result = score_result(std::move(result), annotations);
        result["benchmark_version"] = kMeetingBenchmarkVersion;
        result["provenance"] = provenance;
        write_json(result_path, result);
        context.out << "  done: draft "
                    << py::format_fixed(result["draft_score"]["wer"].get<double>() * 100, 2)
                    << "%, final "
                    << py::format_fixed(result["final_score"]["wer"].get<double>() * 100, 2) << "%"
                    << (result["offline_score"].is_object()
                            ? ", offline " +
                                  py::format_fixed(
                                      result["offline_score"]["wer"].get<double>() * 100, 2) +
                                  "%"
                            : std::string())
                    << ", RTF " << py::format_fixed(result["rtf"].get<double>(), 3) << std::endl;
        results.push_back(std::move(result));
    }
    const Json summary = meeting_summary(results, "parakeet-v3", language, args.target_sec,
                                         args.max_sec, args.offline_pass);
    write_meeting_summary(run_dir, summary);
    write_json(args.output.value_or("out/perf/bench-meeting.json"),
               make_report("meeting", summary,
                           report_provenance(context.source_root, lookup.files->runtime_dir,
                                             lookup.files->model_path)));
    context.out << summary.dump(2) << '\n';
    return 0;
}

// --- asr ---------------------------------------------------------------------------------------

AsrBackendLoader parakeet_loader(CommandContext& context, const EngineFiles& files,
                                 std::shared_ptr<std::unique_ptr<BenchEngine>> holder) {
    return [&context, files, holder] {
        *holder = load(context, files);
        BenchEngine& engine = **holder;
        AsrBackend backend;
        backend.device = files.device;
        backend.device_info = engine.readiness().device_info;
        backend.identity = Json{{"runtime", path_to_utf8(files.runtime_dir)},
                                {"model", path_to_utf8(files.model_path)},
                                {"model_sha256", file_identity(files.model_path)}};
        backend.transcribe = [&engine](const std::filesystem::path& path) {
            RecognitionOptions options;
            return speech::decode_file(engine.service(), path, options).transcript.text;
        };
        backend.cleanup = [holder] {
            holder->reset();
        };
        return backend;
    };
}

int asr(const std::vector<std::string>& argv, CommandContext& context) {
    const Arguments args(argv, {}, with_engine({"--reference", "--runs", "--output"}));
    if (args.positionals().empty()) {
        throw UsageError("the following arguments are required: audio");
    }
    AsrArgs run_args;
    for (const std::string& audio : args.positionals()) {
        run_args.audio.push_back(path_from_utf8(audio));
    }
    for (const std::string& reference : args.all("--reference")) {
        run_args.references.push_back(path_from_utf8(reference));
    }
    run_args.runs = args.integer("--runs").value_or(3);
    if (run_args.runs < 1) {
        throw UsageError("--runs must be at least 1");
    }
    run_args.requested_device = device_of(args);
    const EngineLookup lookup = find_engine_files(args.last("--runtime"), args.last("--model"),
                                                  context.env, run_args.requested_device);
    if (!lookup.files) {
        context.err << lookup.missing << '\n';
        return 2;
    }
    const auto holder = std::make_shared<std::unique_ptr<BenchEngine>>();
    const AsrRun run = run_asr(run_args, parakeet_loader(context, *lookup.files, holder));
    const auto output = path_from_utf8(args.last("--output").value_or("out/perf/bench-asr.json"));
    write_json(output, make_report("asr", run.metrics,
                                   report_provenance(context.source_root, lookup.files->runtime_dir,
                                                     lookup.files->model_path)));
    context.out << run.metrics.dump(2) << '\n';
    return run.failed ? 1 : 0;
}

int asr_corpus(const std::vector<std::string>& argv, CommandContext& context) {
    const Arguments args(argv, {}, with_engine({"--output"}));
    if (args.positionals().size() != 1) {
        throw UsageError("the following arguments are required: manifest");
    }
    CorpusArgs run_args;
    run_args.manifest = path_from_utf8(args.positionals().front());
    run_args.output =
        path_from_utf8(args.last("--output").value_or("out/perf/bench-asr-corpus.json"));
    run_args.requested_device = device_of(args);
    run_args.source_root = context.source_root;
    const EngineLookup lookup = find_engine_files(args.last("--runtime"), args.last("--model"),
                                                  context.env, run_args.requested_device);
    if (!lookup.files) {
        context.err << lookup.missing << '\n';
        return 2;
    }
    const auto holder = std::make_shared<std::unique_ptr<BenchEngine>>();
    const int code = run_asr_corpus(run_args, parakeet_loader(context, *lookup.files, holder),
                                    context.mutable_env);
    context.out << "Wrote " << path_to_utf8(run_args.output) << '\n';
    return code;
}

} // namespace

LivePreviewArgs parse_live_preview_args(const std::vector<std::string>& argv) {
    const Arguments args(argv, {},
                         with_engine({"--output", "--chunk", "--overlap", "--repeats", "--limit"}));
    if (args.positionals().size() != 1) {
        throw UsageError(args.positionals().empty()
                             ? "the following arguments are required: manifest"
                             : "unrecognized arguments: " + args.positionals()[1]);
    }
    LivePreviewArgs out;
    out.manifest = path_from_utf8(args.positionals().front());
    if (const auto output = args.last("--output")) {
        out.output = path_from_utf8(*output);
    }
    out.chunk = args.number("--chunk", out.chunk);
    out.overlap = args.number("--overlap", out.overlap);
    out.repeats = args.integer("--repeats").value_or(out.repeats);
    out.limit = args.integer("--limit");
    out.runtime = args.last("--runtime");
    out.model = args.last("--model");
    out.device = device_of(args);
    if (out.repeats < 1 || (out.limit && *out.limit < 1)) {
        throw UsageError("repeats and limit must be positive");
    }
    if (!std::isfinite(out.chunk) || !std::isfinite(out.overlap)) {
        throw UsageError("chunk and overlap must be finite");
    }
    if (out.chunk <= 0 || !(0 <= out.overlap && out.overlap < out.chunk)) {
        throw UsageError("chunk must be positive and overlap must be in [0, chunk)");
    }
    return out;
}

MeetingArgs parse_meeting_args(const std::vector<std::string>& argv) {
    const Arguments args(argv, {"--offline-pass", "--no-offline-pass", "--force"},
                         with_engine({"--data-dir", "--results-dir", "--language", "--meetings",
                                      "--max-meetings", "--target-sec", "--max-sec", "--output"}));
    if (!args.positionals().empty()) {
        throw UsageError("unrecognized arguments: " + args.positionals().front());
    }
    MeetingArgs out;
    if (const auto value = args.last("--data-dir")) {
        out.data_dir = path_from_utf8(*value);
    }
    if (const auto value = args.last("--results-dir")) {
        out.results_dir = path_from_utf8(*value);
    }
    out.language = args.last("--language").value_or(out.language);
    if (const auto value = args.last("--meetings"); value && !value->empty()) {
        out.meetings = split_commas(*value);
    }
    out.max_meetings = args.integer("--max-meetings");
    out.target_sec = args.number("--target-sec", out.target_sec);
    out.max_sec = args.number("--max-sec", out.max_sec);
    if (args.flag("--no-offline-pass")) {
        out.offline_pass = false;
    }
    if (args.flag("--offline-pass")) {
        out.offline_pass = true;
    }
    out.force = args.flag("--force");
    out.runtime = args.last("--runtime");
    out.model = args.last("--model");
    out.device = device_of(args);
    if (const auto value = args.last("--output")) {
        out.output = path_from_utf8(*value);
    }
    return out;
}

int run_command(const std::vector<std::string>& args, CommandContext& context) {
    if (args.empty() || args.front() == "-h" || args.front() == "--help") {
        context.out << kUsage << '\n';
        return args.empty() ? 2 : 0;
    }
    const std::string& command = args.front();
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    try {
        if (command == "live-preview") {
            return live_preview(rest, context);
        }
        if (command == "live-preview-report") {
            return live_preview_report(rest, context);
        }
        if (command == "cleanup-levels") {
            return cleanup_levels(rest, context);
        }
        if (command == "meeting") {
            return meeting(rest, context);
        }
        if (command == "asr") {
            return asr(rest, context);
        }
        if (command == "asr-corpus") {
            return asr_corpus(rest, context);
        }
        throw UsageError("argument command: invalid choice: " + py::repr(command));
    } catch (const UsageError& error) {
        context.err << kUsage << "\nopenwhisper-cpp-bench: error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        context.err << error.what() << '\n';
        return 1;
    }
}

} // namespace fv::bench
