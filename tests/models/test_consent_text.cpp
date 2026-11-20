// The copy of the download and runtime prompts, replayed against the vectors of
// ui_qt.dialogs.hf_consent_dialog, ui_qt.dialogs.required_runtime_dialog and the batch dialog and
// confirmations of ui_qt.dialogs.settings_downloads.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "models/consent_text.h"
#include "models/hf_access_policy.h"
#include "models_test_support.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;
namespace golden = fv::golden;

namespace {

std::optional<std::string> optional_text(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional<std::string>(value.get<std::string>());
}

} // namespace

TEST_CASE("the consent title carries the bundled size", "[consent_text]") {
    CHECK(consent_title("parakeet-v3") == "Download \"parakeet-v3\" (about 714 MB)?");
    CHECK(consent_title("unknown-model") == "Download \"unknown-model\" model?");
}

TEST_CASE("runtime needs ask for the GPU runtime only when a runtime is missing",
          "[consent_text]") {
    Host host = host_for("win_amd64", gpu("NVIDIA GeForce RTX 2060", 6144, std::pair{7, 5}));
    RuntimeNeeds needs = runtime_needs("parakeet-v3", Json::object(), host);
    CHECK(needs.runtime == "asr-nvidia-cpu");
    // The Vulkan loader is not asked on Windows: the card's runtime is CUDA.
    CHECK(needs.gpu_runtime == "asr-nvidia-cuda");
    host.is_installed = [](std::string_view id) {
        return id == "asr-nvidia-cpu";
    };
    needs = runtime_needs("parakeet-v3", Json::object(), host);
    CHECK_FALSE(needs.runtime);
    CHECK_FALSE(needs.gpu_runtime);
}

TEST_CASE("runtime sizes come from this platform's catalog entry", "[consent_text]") {
    const RuntimeSizes cpu = runtime_sizes("asr-nvidia-cpu", host_for("win_amd64"));
    CHECK(cpu.download == 4'730'421);
    CHECK(cpu.on_disk == 26'420'434);
    const RuntimeSizes none = runtime_sizes("asr-nvidia-cpu", host_for(std::nullopt));
    CHECK(none.download == 0);
    CHECK(none.on_disk == 0);
}

TEST_CASE("ui_qt.dialogs.hf_consent_dialog._title_text vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.hf_consent_dialog", "_title_text", [](const golden::Case& c) {
        CHECK(consent_title(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("ui_qt.dialogs.hf_consent_dialog._body_text vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.hf_consent_dialog", "_body_text", [](const golden::Case& c) {
        RuntimeNeeds needs;
        needs.runtime = optional_text(c.args()[3]);
        needs.gpu_runtime = optional_text(c.args()[4]);
        CHECK(golden::equal_rebranded(
            c.result<std::string>(),
            consent_body(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<bool>(2), needs)));
    });
}

TEST_CASE("ui_qt.dialogs.required_runtime_dialog.RequiredRuntimeDialog vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.required_runtime_dialog", "RequiredRuntimeDialog",
                     [](const golden::Case& c) {
                         const std::string model = c.arg<std::string>(0);
                         const std::string component = c.arg<std::string>(1);
                         const Json actual = Json::array(
                             {kRequiredRuntimeWindowTitle, required_runtime_title(component),
                              required_runtime_body(model, component, c.arg<std::int64_t>(2))});
                         CHECK(golden::equal(c.result<Json>(), actual));
                     });
}

TEST_CASE("ui_qt.dialogs.required_runtime_dialog.GpuRuntimeDialog vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.required_runtime_dialog", "GpuRuntimeDialog", [](const golden::Case& c) {
            const std::string model = c.arg<std::string>(0);
            const std::string gpu_component = c.arg<std::string>(1);
            const std::string cpu_component = c.arg<std::string>(2);
            std::optional<NvidiaGpu> card;
            if (const Json& spec = c.args()[3]; !spec.is_null()) {
                std::optional<std::pair<int, int>> capability;
                if (!spec.at(2).is_null()) {
                    capability =
                        std::pair{spec.at(2).at(0).get<int>(), spec.at(2).at(1).get<int>()};
                }
                card =
                    gpu(spec.at(0).get<std::string>(), spec.at(1).get<std::int64_t>(), capability);
            }
            const Json& sizes = c.args()[4];
            const auto sizes_of = [&sizes](const std::string& id) {
                return RuntimeSizes{sizes.at(id).at(0).get<std::int64_t>(),
                                    sizes.at(id).at(1).get<std::int64_t>()};
            };
            const Json actual =
                Json::array({kGpuRuntimeWindowTitle, gpu_runtime_title(model, card),
                             gpu_runtime_body(model, gpu_component, cpu_component, card,
                                              sizes_of(gpu_component), sizes_of(cpu_component))});
            CHECK(golden::equal(c.result<Json>(), actual));
        });
}

TEST_CASE("ui_qt.dialogs.settings_downloads.BatchDownloadDialog vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.settings_downloads", "BatchDownloadDialog", [](const golden::Case& c) {
            const auto names = c.arg<std::vector<std::string>>(0);
            const Json actual{
                {"window_title", kBatchWindowTitle},
                {"title", batch_download_title(names.size())},
                {"list", batch_model_list(names)},
                {"total", batch_total_text(names)},
                {"buttons", Json::array({"Cancel", batch_download_button(names.size())})}};
            CHECK(golden::equal(c.result<Json>(), actual));
        });
}

TEST_CASE("ui_qt.dialogs.settings_downloads.delete_confirmation vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.settings_downloads", "delete_confirmation", [](const golden::Case& c) {
            const Confirmation asked = delete_model_confirmation(c.arg<std::string>(0));
            CHECK(golden::equal(c.result<Json>(), Json::array({asked.title, asked.text})));
        });
}

TEST_CASE("ui_qt.dialogs.settings_downloads.remove_confirmation vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.settings_downloads", "remove_confirmation", [](const golden::Case& c) {
            const Confirmation asked =
                remove_component_confirmation(c.arg<std::string>(0), c.arg<std::int64_t>(1));
            CHECK(golden::equal(c.result<Json>(), Json::array({asked.title, asked.text})));
        });
}

TEST_CASE("the batch note names the queue's stop control", "[consent_text]") {
    CHECK(std::string(batch_note()).find("Settings → Downloads") != std::string::npos);
    CHECK(std::string(consent_storage_note()).ends_with("works fully offline."));
}
