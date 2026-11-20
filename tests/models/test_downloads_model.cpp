// The Qt-free state of Settings > Downloads: row states, sort and filter, the selection bar, the
// download queue's messages, the rail, the tab titles and the storage summary, replayed against
// the vectors of ui_qt.dialogs.settings_downloads, ui_qt.widgets.model_row_widget and
// ui_qt.widgets.component_row_widget.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "models/downloads_model.h"
#include "uimath/fuzzy_match.h"

using namespace fv::models;
using fv::Json;
namespace golden = fv::golden;

namespace {

const FuzzyMatch kFuzzy = [](std::string_view query, std::string_view target) {
    return fv::fuzzy_match(query, target);
};

double number(const Json& value) {
    if (value.is_object()) {
        const std::string tag = value.at("$float").get<std::string>();
        if (tag == "inf") {
            return std::numeric_limits<double>::infinity();
        }
        if (tag == "-inf") {
            return -std::numeric_limits<double>::infinity();
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    return value.get<double>();
}

Json number_json(double value) {
    if (std::isinf(value)) {
        return Json{{"$float", value > 0 ? "inf" : "-inf"}};
    }
    return value;
}

Json optional_json(const std::optional<std::string>& text) {
    return text ? Json(*text) : Json();
}

CachedModelInfo cached(std::string repo, std::int64_t size) {
    CachedModelInfo info;
    info.repo_id = std::move(repo);
    info.size_on_disk = size;
    info.path = "/models";
    return info;
}

ComponentState state_from(std::string_view name) {
    for (const ComponentState state :
         {ComponentState::NotInstalled, ComponentState::External, ComponentState::Installed,
          ComponentState::UpdateAvailable, ComponentState::Incompatible, ComponentState::Broken}) {
        if (state_name(state) == name) {
            return state;
        }
    }
    throw std::invalid_argument("unknown component state " + std::string(name));
}

ComponentInfo component_info(const Json& fields, std::string default_summary) {
    ComponentInfo info;
    info.component_id = "asr-nvidia-cuda";
    info.display_name = fields.value("display_name", std::string("NVIDIA Speech GPU"));
    info.summary = fields.value("summary", std::move(default_summary));
    info.state = state_from(fields.at("state").get<std::string>());
    info.download_bytes = fields.value("download_bytes", std::int64_t{0});
    info.install_bytes = fields.value("install_bytes", std::int64_t{0});
    info.reason = fields.value("reason", std::string());
    return info;
}

const StorageSummary kSummary{1, 2, 714456704, {{"Parakeet", 714456704}}};

// ComponentRowWidget as the vectors observe it, driven by component_row_state and
// component_row_progress the way the widget applies them.
struct ComponentRowSim {
    std::string name = "asr-nvidia-cuda";
    std::string summary;
    std::string badge_text;
    Json badge_tone;
    std::string size_text;
    bool progress_visible = false;
    bool progress_indeterminate = false;
    int progress_value = 0;
    bool install_visible = true;
    std::string install_text = "Install";
    bool install_enabled = true;
    std::string install_tooltip;
    bool remove_visible = true;
    bool remove_enabled = true;
    bool guard_active = false;
    bool installing = false;

    void update(const ComponentInfo& info, bool now_installing) {
        const ComponentRowState state = component_row_state(info, now_installing, installing);
        installing = now_installing;
        name = state.name;
        summary = state.summary;
        badge_text = state.badge_text;
        badge_tone = state.badge_tone;
        if (state.size_text) {
            size_text = *state.size_text;
        }
        progress_visible = state.progress_visible;
        if (!state.progress_visible) {
            progress_value = 0;
        }
        if (state.install_visible) {
            install_visible = *state.install_visible;
        }
        if (state.install_text) {
            install_text = *state.install_text;
        }
        if (state.install_enabled) {
            install_enabled = *state.install_enabled;
        }
        if (state.install_tooltip) {
            install_tooltip = *state.install_tooltip;
        }
        remove_visible = state.remove_visible;
        switch (state.remove_guard) {
        case RemoveGuard::Start:
            remove_enabled = false;
            guard_active = true;
            break;
        case RemoveGuard::Release:
            if (!guard_active) {
                remove_enabled = true;
            }
            break;
        case RemoveGuard::Keep:
            break;
        }
    }

    void progress(std::string_view phase, std::int64_t done, std::int64_t total) {
        if (!installing) {
            return;
        }
        badge_text = component_phase_badge(phase);
        badge_tone = "downloading";
        const RowProgress progress = component_row_progress(phase, done, total);
        progress_indeterminate = progress.indeterminate;
        if (!progress.indeterminate) {
            progress_value = progress.percent;
        }
        if (progress.size_text) {
            size_text = *progress.size_text;
        }
    }

    void release() {
        guard_active = false;
        remove_enabled = true;
    }

    void check(const Json& expected) const {
        CHECK(expected.at("name") == name);
        CHECK(expected.at("summary") == summary);
        CHECK(expected.at("badge_text") == badge_text);
        CHECK(expected.at("badge_tone") == badge_tone);
        CHECK(expected.at("size_text") == size_text);
        CHECK(expected.at("progress_visible") == progress_visible);
        CHECK(expected.at("progress_indeterminate") == progress_indeterminate);
        if (!progress_indeterminate) {
            CHECK(expected.at("progress_value") == progress_value);
        }
        CHECK(expected.at("install_visible") == install_visible);
        CHECK(expected.at("install_text") == install_text);
        CHECK(expected.at("install_enabled") == install_enabled);
        CHECK(expected.at("install_tooltip") == install_tooltip);
        CHECK(expected.at("remove_visible") == remove_visible);
        CHECK(expected.at("remove_enabled") == remove_enabled);
        CHECK(expected.at("guard_active") == guard_active);
    }
};

} // namespace

// --- New tests ---

TEST_CASE("model rows are the speech models in catalog order", "[downloads_model]") {
    CHECK(model_row_names() == std::vector<std::string>{"parakeet-v3", "orukeet-v0.1"});
    CHECK(backend_filter_values() == std::vector<std::string>{"all", "parakeet"});
    CHECK(backend_filter_index("parakeet") == 1);
    CHECK(backend_filter_index("local_whisper") == -1);
}

TEST_CASE("row facts take the cached size, else the bundled estimate", "[downloads_model]") {
    const ModelRowFacts missing = model_row_facts("parakeet-v3", std::nullopt);
    CHECK(missing.repo_id == "nvidia/parakeet-tdt-0.6b-v3");
    CHECK(missing.backend == "parakeet");
    CHECK_FALSE(missing.is_cached);
    CHECK(missing.sort_size_bytes == 714'000'000);
    const ModelRowFacts present = model_row_facts("parakeet-v3", cached("x", 713'975'456));
    CHECK(present.is_cached);
    CHECK(present.sort_size_bytes == 713'975'456);
    CHECK(std::isinf(model_row_facts("unknown-model", std::nullopt).sort_size_bytes));
}

TEST_CASE("the filter matches name or repository, backend and status", "[downloads_model]") {
    const ModelRowFacts row = model_row_facts("parakeet-v3", std::nullopt);
    CHECK(row_visible(row, "", "all", "all", kFuzzy));
    CHECK(row_visible(row, "  NVIDIA ", "all", "all", kFuzzy));
    CHECK_FALSE(row_visible(row, "zzz", "all", "all", kFuzzy));
    CHECK(row_visible(row, "", "parakeet", "not_downloaded", kFuzzy));
    CHECK_FALSE(row_visible(row, "", "local_whisper", "all", kFuzzy));
    CHECK_FALSE(row_visible(row, "", "all", "downloaded", kFuzzy));
}

TEST_CASE("the selection bar is locked while the inventory is read", "[downloads_model]") {
    SelectionBar bar = selection_summary({{714'000'000}, true, false, false});
    CHECK(bar.download_enabled);
    lock_while_checking(bar);
    CHECK_FALSE(bar.download_enabled);
    CHECK_FALSE(bar.select_all_enabled);
    CHECK_FALSE(bar.download_all_enabled);
    CHECK(bar.clear_enabled);
}

TEST_CASE("models in use follow the selected backend and the loaded model", "[downloads_model]") {
    const Json orukeet = {{"selected_model", "parakeet"},
                          {"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}};
    ModelsInUse in_use = models_in_use(orukeet, "parakeet-v3", "parakeet-v3");
    CHECK(in_use.dictation == "orukeet-v0.1");
    CHECK(in_use.meeting == "parakeet-v3");
    CHECK(usage_text("orukeet-v0.1", in_use.dictation, in_use.meeting) == "Dictation");
    CHECK(usage_text("parakeet-v3", in_use.dictation, in_use.meeting) == "Meetings");
    // Another backend: dictation is Auto, which is the model the engine has loaded.
    in_use = models_in_use(Json{{"selected_model", "api"}}, "", "parakeet-v3");
    CHECK(in_use.dictation == "parakeet-v3");
    in_use = models_in_use(Json{{"selected_model", "api"}}, "", std::nullopt);
    CHECK(in_use.dictation == "auto");
    CHECK(models_in_use(Json::object(), "", std::nullopt).dictation == "parakeet-v3");
}

TEST_CASE("the storage summary counts downloaded speech models by backend", "[downloads_model]") {
    CachedModelsSnapshot::Inventory inventory;
    inventory.emplace("oruk/orukeet", cached("oruk/orukeet", 714'456'704));
    inventory.emplace("someone/else", cached("someone/else", 5));
    const StorageSummary summary = storage_summary(inventory);
    CHECK(summary.downloaded == 1);
    CHECK(summary.total == 2);
    CHECK(summary.bytes == 714'456'704);
    REQUIRE(summary.by_backend.size() == 1);
    CHECK(summary.by_backend[0] == std::pair<std::string, std::int64_t>{"Parakeet", 714'456'704});
    CHECK(downloads_rail_value(DownloadQueue(), false, summary) == "1 of 2 · 714 MB");
    CHECK(storage_summary(CachedModelsSnapshot::Inventory()).bytes == 0);
}

TEST_CASE("a queued row is one waiting behind the download", "[downloads_model]") {
    DownloadQueue queue;
    const std::vector<std::string> plan{"parakeet-v3", "orukeet-v0.1"};
    queue.begin_batch(plan, {});
    queue.set_downloading("parakeet-v3");
    CHECK(queue.slot_busy());
    CHECK_FALSE(queue.is_queued("parakeet-v3"));
    CHECK(queue.is_queued("orukeet-v0.1"));
    CHECK(delete_result_message("parakeet-v3", true, "") == "Deleted \"parakeet-v3\"");
    CHECK(delete_result_message("parakeet-v3", false, "locked") == "Could not delete: locked");
}

// --- ui_qt.dialogs.settings_downloads vectors ---

TEST_CASE("ui_qt.dialogs.settings_downloads.sorted_rows vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_downloads", "sorted_rows", [](const golden::Case& c) {
        std::vector<ModelRowFacts> rows;
        for (const Json& spec : c.args()[1]) {
            ModelRowFacts row;
            row.model_name = spec.at(0).get<std::string>();
            row.backend = spec.at(1).get<std::string>();
            row.is_cached = spec.at(2).get<bool>();
            row.sort_size_bytes = number(spec.at(3));
            rows.push_back(std::move(row));
        }
        Json order = Json::array();
        for (const ModelRowFacts& row : sorted_rows(rows, c.arg<std::string>(0))) {
            order.push_back(row.model_name);
        }
        CHECK(golden::equal(c.result<Json>(), order));
    });
}

TEST_CASE("ui_qt.dialogs.settings_downloads._update_selection_bar vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_downloads", "_update_selection_bar",
                     [](const golden::Case& c) {
                         SelectionBarInput input;
                         for (const Json& size : c.args()[0]) {
                             input.selected_sizes.push_back(number(size));
                         }
                         input.any_missing = c.arg<bool>(1);
                         input.busy = c.arg<bool>(2);
                         input.downloads_blocked = c.arg<bool>(3);
                         const SelectionBar bar = selection_summary(input);
                         const Json actual{{"summary", bar.summary},
                                           {"download_text", bar.download_text},
                                           {"download_enabled", bar.download_enabled},
                                           {"download_tooltip", bar.download_tooltip},
                                           {"clear_enabled", bar.clear_enabled},
                                           {"select_all_enabled", bar.select_all_enabled},
                                           {"download_all_enabled", bar.download_all_enabled},
                                           {"download_all_visible", bar.download_all_visible}};
                         CHECK(golden::equal(c.result<Json>(), actual));
                     });
}

TEST_CASE("ui_qt.dialogs.settings_downloads.queue vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_downloads", "queue", [](const golden::Case& c) {
        DownloadQueue queue;
        std::string message;
        const Json& expected = c.result<Json>();
        const auto is_row = [](std::string_view name) {
            return name == "parakeet-v3" || name == "orukeet-v0.1";
        };
        std::size_t index = 0;
        for (const Json& step : c.args()[0]) {
            const std::string name = step.at(0).get<std::string>();
            bool rail_changed = false;
            if (name == "set_downloading") {
                message = queue.set_downloading(step.at(1).get<std::string>());
            } else if (name == "progress") {
                const auto progress = queue.set_download_progress(step.at(1).get<std::string>(),
                                                                  step.at(2).get<std::int64_t>(),
                                                                  step.at(3).get<std::int64_t>());
                if (progress.message) {
                    message = *progress.message;
                }
                rail_changed = progress.rail_changed;
            } else if (name == "finish_download") {
                if (auto text = queue.finish_download(step.at(1).get<std::string>(),
                                                      step.at(2).get<bool>())) {
                    message = *text;
                }
            } else if (name == "begin_batch") {
                const auto names = step.at(1).get<std::vector<std::string>>();
                message = queue.begin_batch(names, is_row);
            } else if (name == "finish_batch") {
                message = queue.finish_batch(step.at(1).get<int>(), step.at(2).get<int>());
            }
            const Json& want = expected.at(index++);
            INFO("step " << index << " " << step.dump());
            CHECK(want.at("message") == message);
            CHECK(want.at("rail_changed") == rail_changed);
            CHECK(golden::equal(want.at("downloading"), optional_json(queue.downloading_model())));
            CHECK(golden::equal(want.at("fraction"), queue.download_fraction()
                                                         ? Json(*queue.download_fraction())
                                                         : Json()));
            CHECK(golden::equal(want.at("queue"), Json(queue.batch_queue())));
            CHECK(want.at("done") == queue.batch_done());
            CHECK(want.at("failed") == queue.batch_failed());
            CHECK(want.at("rail") == downloads_rail_value(queue, false, kSummary));
        }
    });
}

TEST_CASE("ui_qt.dialogs.settings_downloads.rail_value vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_downloads", "rail_value", [](const golden::Case& c) {
        std::optional<std::string> downloading;
        if (!c.args()[0].is_null()) {
            downloading = c.arg<std::string>(0);
        }
        std::optional<double> fraction;
        if (!c.args()[1].is_null()) {
            fraction = c.arg<double>(1);
        }
        StorageSummary summary;
        summary.downloaded = c.arg<int>(3);
        summary.total = c.arg<int>(4);
        summary.bytes = c.arg<std::int64_t>(5);
        CHECK(downloads_rail_value(downloading, fraction, c.arg<bool>(2), summary) ==
              c.result<std::string>());
    });
}

TEST_CASE("ui_qt.dialogs.settings_downloads._show_cache_totals vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.settings_downloads", "_show_cache_totals", [](const golden::Case& c) {
            const bool checking = c.arg<bool>(0);
            const auto sizes = c.arg<std::vector<std::int64_t>>(1);
            std::int64_t bytes = 0;
            for (const std::int64_t size : sizes) {
                bytes += size;
            }
            const int downloaded = static_cast<int>(sizes.size());
            const int rows = c.arg<int>(2);
            const Json actual = Json::array(
                {models_tab_title(checking, downloaded, rows),
                 models_stats_text(checking, downloaded, rows, bytes, c.arg<std::string>(3))});
            CHECK(golden::equal(c.result<Json>(), actual));
        });
}

TEST_CASE("ui_qt.dialogs.settings_downloads.refresh_components vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.settings_downloads", "refresh_components", [](const golden::Case& c) {
            std::vector<ComponentInfo> infos;
            for (const Json& fields : c.args()[0]) {
                infos.push_back(component_info(fields, ""));
            }
            CHECK(golden::equal(c.result<Json>(), optional_json(components_tab_title(infos))));
        });
}

TEST_CASE("ui_qt.dialogs.settings_downloads._usage_for vectors", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_downloads", "_usage_for", [](const golden::Case& c) {
        CHECK(usage_text(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<std::string>(2)) ==
              c.result<std::string>());
    });
}

TEST_CASE("ui_qt.dialogs.settings_downloads._current_cache_info vectors", "[golden]") {
    golden::for_each(
        "ui_qt.dialogs.settings_downloads", "_current_cache_info", [](const golden::Case& c) {
            std::optional<CachedModelInfo> info;
            if (!c.args()[0].is_null()) {
                const Json& spec = c.args()[0];
                info = cached(spec.at(0).get<std::string>(), spec.at(1).get<std::int64_t>());
                info->path = spec.at(2).get<std::string>();
                info->revisions = spec.at(3).get<std::vector<std::string>>();
            }
            const std::string revision = c.args()[1].is_null() ? "" : c.arg<std::string>(1);
            const auto result = current_cache_info(info, revision);
            Json actual;
            if (result) {
                actual = Json::array({result->repo_id, result->size_on_disk,
                                      result->path.generic_string(), result->revisions});
            }
            CHECK(golden::equal(c.result<Json>(), actual));
        });
}

// --- ui_qt.widgets.model_row_widget vectors ---

TEST_CASE("ui_qt.widgets.model_row_widget.update_state vectors", "[golden]") {
    golden::for_each("ui_qt.widgets.model_row_widget", "update_state", [](const golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        ModelRowInput input;
        if (!c.args()[1].is_null()) {
            input.info = cached("repo", c.arg<std::int64_t>(1));
        }
        input.is_active = c.arg<bool>(2);
        input.is_loaded = c.arg<bool>(3);
        input.downloading = c.arg<bool>(4);
        input.downloads_blocked = c.arg<bool>(5);
        input.download_slot_busy = c.arg<bool>(6);
        input.queued = c.arg<bool>(7);
        input.selection_enabled = c.arg<bool>(8);
        const ModelRowState s = model_row_state(name, input);
        const Json actual{{"active", s.active_style},
                          {"size_text", s.size_text},
                          {"size_muted", s.size_muted},
                          {"sort_size_bytes", number_json(s.sort_size_bytes)},
                          {"badge_text", s.badge_text},
                          {"badge_tone", s.badge_tone},
                          {"badge_visible", s.badge_visible},
                          {"progress_visible", s.progress_visible},
                          {"checkbox_visible", s.checkbox_visible},
                          {"checkbox_enabled", s.checkbox_enabled},
                          {"download_visible", s.download_visible},
                          {"download_enabled", s.download_enabled},
                          {"download_tooltip", s.download_tooltip},
                          {"set_active_visible", s.set_active_visible},
                          {"set_active_enabled", s.set_active_enabled},
                          {"delete_visible", s.delete_visible},
                          {"delete_enabled", s.delete_enabled},
                          {"delete_tooltip", s.delete_tooltip}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("ui_qt.widgets.model_row_widget.set_progress vectors", "[golden]") {
    golden::for_each("ui_qt.widgets.model_row_widget", "set_progress", [](const golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        ModelRowInput input;
        input.downloading = true;
        const ModelRowState state = model_row_state(name, input);
        const RowProgress progress =
            model_row_progress(c.arg<std::int64_t>(1), c.arg<std::int64_t>(2));
        const Json actual{{"indeterminate", progress.indeterminate},
                          {"percent", progress.indeterminate ? Json() : Json(progress.percent)},
                          {"size_text", progress.size_text.value_or(state.size_text)},
                          {"progress_visible", true}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("ui_qt.widgets.model_row_widget._model_summary vectors", "[golden]") {
    golden::for_each("ui_qt.widgets.model_row_widget", "_model_summary", [](const golden::Case& c) {
        CHECK(model_row_summary(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("ui_qt.widgets.model_row_widget.matches_filter vectors", "[golden]") {
    golden::for_each("ui_qt.widgets.model_row_widget", "matches_filter", [](const golden::Case& c) {
        const ModelRowFacts row = model_row_facts(c.arg<std::string>(0), std::nullopt);
        CHECK(matches_filter(c.arg<std::string>(1), row, kFuzzy) == c.result<bool>());
    });
}

// --- ui_qt.widgets.component_row_widget vectors ---

TEST_CASE("ui_qt.widgets.component_row_widget.steps vectors", "[golden]") {
    golden::for_each("ui_qt.widgets.component_row_widget", "steps", [](const golden::Case& c) {
        const Json& expected = c.result<Json>();
        ComponentRowSim row;
        std::size_t index = 0;
        {
            INFO("initial");
            row.check(expected.at(index++));
        }
        for (const Json& step : c.args()[0]) {
            const std::string name = step.at(0).get<std::string>();
            if (name == "state") {
                row.update(component_info(step.at(1), "NVIDIA GPU runtime."),
                           step.at(2).get<bool>());
            } else if (name == "progress") {
                row.progress(step.at(1).get<std::string>(), step.at(2).get<std::int64_t>(),
                             step.at(3).get<std::int64_t>());
            } else if (name == "release") {
                row.release();
            }
            INFO("step " << index << " " << step.dump());
            row.check(expected.at(index++));
        }
    });
}
