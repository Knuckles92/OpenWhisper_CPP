// openwhisper-cpp-bench: OpenWhisper's developer benchmarks (benchmarks/live_preview.py,
// cleanup_levels/run.py, meeting_mode/run.py, scripts/benchmark_local_asr*.py) over fv_bench.
// Qt-free, never staged or installed, and it contacts no host but the cleanup provider a
// cleanup-levels run names. The commands are in bench/commands.h.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/paths.h"
#include "bench/commands.h"
#include "bench/provenance.h"
#include "credentials/dotenv.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "engine/platform.h"
#include "http/client.h"
#include "llm/transcript_cleanup.h"
#include "process/process_env.h"
#include "settings/store.h"
#include "text/cleanup_prompts.h"

namespace {

// Everything a TranscriptCleanup borrows, kept alive by the handle.
struct CleanupParts {
    std::shared_ptr<fv::SettingsStore> settings;
    fv::CredentialStore store{fv::make_platform_backend(), fv::identity()};
    fv::DotEnv dotenv;
    std::unique_ptr<fv::llm::TranscriptCleanup> cleanup;

    CleanupParts(std::shared_ptr<fv::SettingsStore> saved, const std::filesystem::path& env_file)
        : settings(std::move(saved)), dotenv(env_file) {}
};

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> all = fv::utf8_args(argc, argv);
    const std::vector<std::string> args(all.begin() + (all.empty() ? 0 : 1), all.end());
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    auto settings =
        std::make_shared<fv::SettingsStore>(paths.user_file(fv::identity().settings_file));

    fv::bench::CommandContext context{std::cout, std::cerr, fv::system_env(),
                                      fv::system_mutable_env()};
    context.source_root = fv::bench::find_source_root(std::filesystem::current_path());
    context.load_settings = [settings] {
        return *settings->snapshot();
    };
    context.make_cleaner = [settings, &paths](const std::string& provider, const std::string& model,
                                              const std::string& reasoning) {
        auto parts = std::make_shared<CleanupParts>(settings, paths.env_file());
        const fv::llm::KeySources keys{parts->store, fv::system_env(), parts->dotenv};
        fv::llm::CleanupOptions options;
        options.default_prompt = fv::text::legacy_default_prompt();
        parts->cleanup = std::make_unique<fv::llm::TranscriptCleanup>(
            fv::http::make_curl_client(), keys, [settings] { return *settings->snapshot(); },
            provider, model, reasoning, /*defer_client=*/false, std::move(options));
        fv::llm::TranscriptCleanup* cleanup = parts->cleanup.get();
        fv::bench::CleanupClientHandle handle;
        handle.cleaner.cleanup = [cleanup](std::string_view text, std::string_view prompt) {
            return cleanup->cleanup(text, prompt);
        };
        handle.cleaner.last_error = [cleanup] {
            return cleanup->last_error();
        };
        handle.is_available = [cleanup] {
            return cleanup->is_available();
        };
        handle.provider = cleanup->provider();
        handle.model = cleanup->model();
        handle.reasoning = cleanup->reasoning();
        handle.owner = parts;
        return handle;
    };
    return fv::bench::run_command(args, context);
}
