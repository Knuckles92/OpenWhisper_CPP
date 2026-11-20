// tests/test_settings.py: SettingsManager (now SettingsStore) and the hotkey load and save, plus
// the store's own contracts: the file's bytes, the signature cache, watchers, the in-memory
// file other packages test with.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/manual_executor.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "settings_test_support.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::settings_test::InlineExecutor;
using fv::settings_test::LogCapture;
using fv::settings_test::names_in;
using fv::settings_test::read_bytes;
using fv::settings_test::same;
using fv::settings_test::TempDir;
using fv::settings_test::with_os_linesep;
using fv::settings_test::write_bytes;

namespace {

// The file the manager tests use: test_settings.json in a fresh folder.
struct ManagerFixture {
    TempDir temp;
    std::filesystem::path file = temp.path() / "test_settings.json";
    fv::SettingsStore store{file};
};

// Counts the reads of the file on disk it stands for.
class CountingFile final : public fv::SettingsFile {
public:
    explicit CountingFile(std::filesystem::path path)
        : inner_(fv::disk_settings_file(std::move(path))) {}
    const std::filesystem::path& path() const override { return inner_->path(); }
    std::optional<fv::FileSignature> signature() const override { return inner_->signature(); }
    bool exists() const override { return inner_->exists(); }
    std::string read() const override {
        ++reads;
        return inner_->read();
    }
    void replace(std::string_view bytes) override { inner_->replace(bytes); }

    mutable std::atomic<int> reads{0};

private:
    std::unique_ptr<fv::SettingsFile> inner_;
};

} // namespace

TEST_CASE("load hotkey settings default", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_load_hotkey_settings_default
    ManagerFixture f;
    CHECK(same(fv::default_hotkeys(), fv::load_hotkey_settings(f.store)));
}

TEST_CASE("save and load hotkey settings", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_save_and_load_hotkey_settings
    ManagerFixture f;
    const Json hotkeys{{"record_toggle", "f1"}, {"cancel", "f2"}, {"enable_disable", "ctrl+f3"}};
    fv::save_hotkey_settings(f.store, hotkeys);
    CHECK(same(hotkeys, fv::load_hotkey_settings(f.store)));
}

TEST_CASE("load hotkey settings partial", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_load_hotkey_settings_partial
    ManagerFixture f;
    write_bytes(f.file, R"({"hotkeys": {"record_toggle": "f1"}})");
    // The partial table comes back as it is, not merged with the defaults.
    CHECK(same(Json{{"record_toggle", "f1"}}, fv::load_hotkey_settings(f.store)));
}

TEST_CASE("save hotkey settings invalid file", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_save_hotkey_settings_invalid_file
    TempDir temp;
    fv::SettingsStore invalid(temp.path() / "invalid" / "path" / "settings.json");
    CHECK_THROWS(fv::save_hotkey_settings(invalid, Json{{"test", "value"}}));
}

TEST_CASE("the default hotkeys are Windows' table in config order") {
    const Json hotkeys = fv::default_hotkeys();
    std::vector<std::string> order;
    for (const auto& [name, value] : hotkeys.items()) {
        order.push_back(name);
    }
    CHECK(order == std::vector<std::string>{"record_toggle", "cancel", "enable_disable",
                                            "minimize_tray", "meeting_toggle", "command_mode",
                                            "scratchpad_toggle", "cycle_language",
                                            "paste_last_original", "route_record", "walkthrough",
                                            "screenshot", "paste_walkthrough"});
    CHECK(hotkeys.at("record_toggle") == "kp *");
    CHECK(hotkeys.at("cancel") == "kp -");
    CHECK(hotkeys.at("enable_disable") == "ctrl+alt+kp *");
    CHECK(hotkeys.at("minimize_tray") == "ctrl+alt+m");
    CHECK(hotkeys.at("paste_walkthrough") == "ctrl+shift+f12");
}

TEST_CASE("load all settings", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_load_all_settings
    ManagerFixture f;
    const Json settings{{"hotkeys", {{"record_toggle", "f1"}}}, {"other_setting", "value"}};
    write_bytes(f.file, settings.dump());
    CHECK(same(settings, *f.store.snapshot()));
}

TEST_CASE("load all settings empty", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_load_all_settings_empty
    ManagerFixture f;
    CHECK(*f.store.snapshot() == Json::object());
}

TEST_CASE("each whole load is an independent copy of what was saved", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_each_whole_load_is_an_independent_copy_of_what_was_saved
    // OpenWhisper saves 2 ** 70, an int Python keeps exactly; JSON numbers here are int64,
    // uint64 or double, so the largest integer the port keeps exactly is 2 ** 64 - 1.
    ManagerFixture f;
    Json saved = Json::object();
    saved["hotkeys"] = {{"record_toggle", "f1"}};
    saved["dictation_dictionary"] =
        Json::array({{{"term", "Kubernetes"}, {"starred", true}, {"uses", 3}}});
    saved["ratio"] = 0.5;
    saved["count"] = 12;
    saved["big"] = std::uint64_t{18446744073709551615ULL};
    saved["nothing"] = nullptr;
    saved["text"] = "na\xc3\xafve \xe2\x98\x95";
    f.store.save_all(saved);

    // A snapshot is immutable; a caller edits a copy, which leaves the next load as saved.
    Json first = *f.store.snapshot();
    first["hotkeys"]["record_toggle"] = "f9";
    first["dictation_dictionary"][0]["term"] = "changed";
    first["added"] = true;
    const auto second = f.store.snapshot();
    CHECK(same(saved, *second));
    CHECK(second->at("ratio").is_number_float());
    CHECK(second->at("count").is_number_integer());
    CHECK(second->at("big").is_number_unsigned());
    CHECK(second->at("nothing").is_null());
}

TEST_CASE("load settings returns only the saved keys asked for", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_load_settings_returns_only_the_saved_keys_asked_for
    ManagerFixture f;
    f.store.save_all(Json{{"auto_paste", false},
                          {"remote_host_devices", Json::array({{{"id", "a"}}})},
                          {"other", 1}});
    const Json loaded =
        f.store.load_settings({fv::keys::auto_paste.key(), fv::keys::remote_host_devices.key(),
                               fv::keys::host_mode.key()});
    CHECK(same(Json{{"auto_paste", false}, {"remote_host_devices", Json::array({{{"id", "a"}}})}},
               loaded));
    CHECK(fv::keys::auto_paste.read(loaded) == false);
    // Not saved, so its declared default applies.
    CHECK(fv::keys::host_mode.read(loaded) == false);
}

TEST_CASE("load settings copies what it returns", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_load_settings_copies_what_it_returns
    ManagerFixture f;
    f.store.save_all(Json{{"remote_host_devices", Json::array({{{"id", "a"}}})}});
    Json loaded = f.store.load_settings({"remote_host_devices"});
    loaded["remote_host_devices"][0]["id"] = "changed";
    loaded["remote_host_devices"].push_back({{"id", "b"}});
    const Json expected{{"remote_host_devices", Json::array({{{"id", "a"}}})}};
    CHECK(same(expected, f.store.load_settings({"remote_host_devices"})));
    CHECK(same(expected, *f.store.snapshot()));
    CHECK(same(Json::array({{{"id", "a"}}}), f.store.value("remote_host_devices")));
}

TEST_CASE("load settings follows saves and outside edits without rereading between",
          "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_load_settings_follows_saves_and_outside_edits_without_rereading_between
    TempDir temp;
    const auto path = temp.path() / "test_settings.json";
    auto counting = std::make_unique<CountingFile>(path);
    CountingFile& file = *counting;
    fv::SettingsStore store(std::move(counting));
    store.save_all(Json{{"auto_paste", true}});
    file.reads = 0;

    for (int i = 0; i < 5; ++i) {
        CHECK(same(Json{{"auto_paste", true}}, store.load_settings({"auto_paste"})));
        CHECK(store.value("auto_paste") == true);
    }
    CHECK(file.reads == 1);

    store.save_setting("auto_paste", false);
    CHECK(same(Json{{"auto_paste", false}}, store.load_settings({"auto_paste"})));

    const auto replacement = temp.path() / "replacement.json";
    write_bytes(replacement, R"({"auto_paste": true, "added_outside": 1})");
    std::filesystem::rename(replacement, path);
    CHECK(same(Json{{"auto_paste", true}, {"added_outside", 1}},
               store.load_settings({"auto_paste", "added_outside"})));
}

TEST_CASE("get returns the default for a key that is not saved", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_get_returns_the_default_for_a_key_that_is_not_saved
    ManagerFixture f;
    f.store.save_all(Json{{"saved_none", nullptr}});
    CHECK(f.store.value("missing", "fallback") == "fallback");
    CHECK(f.store.value("missing").is_null());
    CHECK(f.store.value("saved_none", "fallback").is_null());
}

TEST_CASE("save all settings", "[test_settings]") {
    // from tests/test_settings.py::TestSettingsManager::test_save_all_settings
    ManagerFixture f;
    const Json settings{{"hotkeys", {{"record_toggle", "f1"}}}, {"window_size", "400x300"}};
    f.store.save_all(settings);
    CHECK(same(settings, Json::parse(read_bytes(f.file))));
}

TEST_CASE("concurrent setting writes preserve every key", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_concurrent_setting_writes_preserve_every_key
    ManagerFixture f;
    std::vector<std::thread> threads;
    for (int index = 0; index < 20; ++index) {
        threads.emplace_back(
            [&f, index] { f.store.save_setting("worker_" + std::to_string(index), index); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto loaded = f.store.snapshot();
    CHECK(loaded->size() == 20);
    for (int index = 0; index < 20; ++index) {
        CHECK(loaded->at("worker_" + std::to_string(index)) == index);
    }
}

TEST_CASE("failed atomic replace preserves previous settings", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_failed_atomic_replace_preserves_previous_settings
    ManagerFixture f;
    const Json original{{"kept", true}};
    f.store.save_all(original);
#if defined(_WIN32)
    // An open handle without delete sharing makes the replace fail, as Python's patched
    // os.replace does.
    std::ifstream holder(f.file, std::ios::binary);
    REQUIRE(holder);
    CHECK_THROWS_AS(f.store.save_setting("new", "value"), std::filesystem::filesystem_error);
    holder.close();
#else
    fv::MemorySettingsStore memory(original);
    memory.file().fail_next_replace("blocked");
    CHECK_THROWS_WITH(memory.save_setting("new", "value"), "blocked");
    CHECK(same(original, *memory.snapshot()));
#endif
    CHECK(same(original, *f.store.snapshot()));
    CHECK(names_in(f.temp.path()) == std::vector<std::string>{"test_settings.json"});
}

TEST_CASE("malformed file is not overwritten by single setting", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_malformed_file_is_not_overwritten_by_single_setting
    ManagerFixture f;
    write_bytes(f.file, "{not-json");
    try {
        f.store.save_setting("new", "value");
        FAIL("the strict read did not refuse the file");
    } catch (const fv::SettingsFileError& error) {
        CHECK(error.python_type() == "JSONDecodeError");
    }
    CHECK(read_bytes(f.file) == "{not-json");
}

TEST_CASE("update settings merges and removes in one transaction", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_update_settings_merges_and_removes_in_one_transaction
    ManagerFixture f;
    f.store.save_all(Json{{"keep", 1}, {"remove", 2}});
    const Json committed = f.store.update(Json{{"added", 3}}, {"remove"});
    CHECK(same(Json{{"keep", 1}, {"added", 3}}, committed));
    CHECK(same(committed, *f.store.snapshot()));
}

TEST_CASE("mutate settings commits structured change and returns result", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_mutate_settings_commits_structured_change_and_returns_result
    ManagerFixture f;
    f.store.save_all(Json{{"profiles", Json::array({{{"id", "a"}}})}});
    std::string result;
    f.store.mutate([&](Json& settings) {
        settings["profiles"].push_back({{"id", "b"}});
        result = "b";
    });
    CHECK(result == "b");
    CHECK(same(Json::array({{{"id", "a"}}, {{"id", "b"}}}), f.store.snapshot()->at("profiles")));
}

TEST_CASE("mutate settings does not write when callback fails", "[test_settings]") {
    // from
    // tests/test_settings.py::TestSettingsManager::test_mutate_settings_does_not_write_when_callback_fails
    ManagerFixture f;
    const Json original{{"keep", true}};
    f.store.save_all(original);
    const std::string before = read_bytes(f.file);
    CHECK_THROWS_WITH(f.store.mutate([](Json& settings) {
        settings["leak"] = true;
        throw std::runtime_error("invalid mutation");
    }),
                      "invalid mutation");
    CHECK(same(original, *f.store.snapshot()));
    CHECK(read_bytes(f.file) == before);
}

TEST_CASE("unreadable settings load as empty and block writes", "[test_settings]") {
    // from tests/test_settings.py::test_unreadable_settings_load_as_empty_and_block_writes
    struct Unreadable {
        std::string content;
        std::string error;
        std::string warning;
    };
    const std::vector<Unreadable> cases{
        {"{not-json", "JSONDecodeError",
         "Failed to load all settings: Expecting property name enclosed in double quotes: "
         "line 1 column 2 (char 1)"},
        {"{\"a\": \"\xff\xfe\"}", "UnicodeDecodeError",
         "Failed to load all settings: 'utf-8' codec can't decode byte 0xff in position 7: "
         "invalid start byte"},
        {"[1, 2]", "ValueError",
         "Failed to load all settings: settings root must be a JSON object"},
        {std::string(200'000, '[') + std::string(200'000, ']'), "RecursionError",
         "Failed to load all settings: maximum recursion depth exceeded while decoding a JSON "
         "array from a unicode string"},
    };
    for (const Unreadable& unreadable : cases) {
        INFO(unreadable.error);
        TempDir temp;
        const auto path = temp.path() / "settings.json";
        write_bytes(path, unreadable.content);
        fv::SettingsStore store(path);
        {
            LogCapture log;
            CHECK(*store.snapshot() == Json::object());
            CHECK(log.contains(fv::log::Level::Warning, unreadable.warning));
        }
        try {
            store.save_setting("new", "value");
            FAIL("the strict read did not refuse the file");
        } catch (const fv::SettingsFileError& error) {
            CHECK(error.python_type() == unreadable.error);
        }
        CHECK(read_bytes(path) == unreadable.content);
    }
}

TEST_CASE("settings path that cannot be read loads as empty", "[test_settings]") {
    // from tests/test_settings.py::test_settings_path_that_cannot_be_read_loads_as_empty
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    std::filesystem::create_directory(path);
    fv::SettingsStore store(path);
    CHECK(*store.snapshot() == Json::object());
    try {
        store.update(Json{{"new", "value"}});
        FAIL("the strict read did not refuse the folder");
    } catch (const fv::SettingsFileError& error) {
#if defined(_WIN32)
        CHECK(error.python_type() == "PermissionError");
        CHECK(std::string(error.what()).starts_with("[Errno 13] Permission denied: '"));
#else
        CHECK(error.python_type() == "IsADirectoryError");
#endif
    }
    CHECK(std::filesystem::is_directory(path));
}

TEST_CASE("the utf-8 check reports what CPython reports") {
    struct Bad {
        std::string bytes;
        std::string message;
    };
    const std::vector<Bad> cases{
        {"{\"a\": \"\xe2\x82\"}", "'utf-8' codec can't decode bytes in position 7-8: "
                                  "invalid continuation byte"},
        {"{\"a\": \"\xe0\x80\x80\"}", "'utf-8' codec can't decode byte 0xe0 in position 7: "
                                      "invalid continuation byte"},
        {"{\"a\": \"\xed\xa0\x80\"}", "'utf-8' codec can't decode byte 0xed in position 7: "
                                      "invalid continuation byte"},
        {"{\"a\": \"\xc0\xaf\"}", "'utf-8' codec can't decode byte 0xc0 in position 7: "
                                  "invalid start byte"},
        {"{\"a\": \"x\"}\xf0\x9f\x98", "'utf-8' codec can't decode bytes in position 10-12: "
                                       "unexpected end of data"},
        {"\xf4\x90\x80\x80{}", "'utf-8' codec can't decode byte 0xf4 in position 0: "
                               "invalid continuation byte"},
    };
    for (const Bad& bad : cases) {
        TempDir temp;
        const auto path = temp.path() / "settings.json";
        write_bytes(path, bad.bytes);
        fv::SettingsStore store(path);
        try {
            store.update(Json::object());
            FAIL("accepted invalid UTF-8");
        } catch (const fv::SettingsFileError& error) {
            CHECK(error.python_type() == "UnicodeDecodeError");
            CHECK(std::string(error.what()) == bad.message);
        }
    }
}

TEST_CASE("text mode newlines and a BOM are read as Python reads them") {
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    write_bytes(path, "{\r\n  \"a\": 1,\r  \"b\": [2]\r\n}\r\n");
    fv::SettingsStore store(path);
    CHECK(same(Json{{"a", 1}, {"b", Json::array({2})}}, *store.snapshot()));
    write_bytes(path, "\xef\xbb\xbf{}");
    try {
        store.update(Json::object());
        FAIL("accepted a BOM");
    } catch (const fv::SettingsFileError& error) {
        CHECK(error.python_type() == "JSONDecodeError");
        CHECK(std::string(error.what()).starts_with("Unexpected UTF-8 BOM"));
    }
}

TEST_CASE("a file the store writes is the bytes Python's json.dump writes") {
    // tests/settings/fixtures/python_settings.json is json.dump(..., indent=2) plus a newline,
    // made by OpenWhisper's Python with non-ASCII text, floats and keys no declaration knows.
    // Git keeps it with "\n" line ends (eol=lf); SettingsManager writes through a text-mode
    // handle, so the file OpenWhisper leaves on Windows has "\r\n", and that is what is compared.
    const auto fixture =
        fv::testkit::source_root() / "tests" / "settings" / "fixtures" / "python_settings.json";
    std::string lf = read_bytes(fixture);
    std::erase(lf, '\r');
    const std::string python = with_os_linesep(lf);
    REQUIRE_FALSE(python.empty());
    TempDir temp;
    const auto path = temp.path() / "openwhisper_cpp_settings.json";
    write_bytes(path, python);
    fv::SettingsStore store(path);
    store.mutate([](Json&) {});
    CHECK(read_bytes(path) == python);
    store.save_all(*store.snapshot());
    CHECK(read_bytes(path) == python);
    store.save_setting("ui_theme", "dark");
    CHECK(read_bytes(path) == python);
}

TEST_CASE("mutate writes even when nothing changed") {
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    fv::SettingsStore store(path);
    CHECK_FALSE(std::filesystem::exists(path));
    store.mutate([](Json&) {});
    CHECK(read_bytes(path) == with_os_linesep("{}\n"));
}

TEST_CASE("lines end as Python's text mode ends them") {
    // What OpenWhisper's Python 3.12 wrote on Windows for {"a": 1, "b": [1, 2]}: the newline
    // json.dump puts in the layout becomes os.linesep.
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    fv::SettingsStore store(path);
    store.save_all(Json{{"a", 1}, {"b", Json::array({1, 2})}});
#if defined(_WIN32)
    CHECK(read_bytes(path) == "{\r\n  \"a\": 1,\r\n  \"b\": [\r\n    1,\r\n    2\r\n  ]\r\n}\r\n");
#else
    CHECK(read_bytes(path) == "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ]\n}\n");
#endif
    // A newline inside a value is escaped by json.dump, never translated.
    store.save_setting("a", "two\nlines");
    CHECK(read_bytes(path).find("\"two\\nlines\"") != std::string::npos);
    CHECK(store.snapshot()->at("a") == "two\nlines");
}

TEST_CASE("a new install is decided when the store is built") {
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    fv::SettingsStore fresh(path);
    CHECK(fresh.is_new_install());
    fresh.save_setting("ui_theme", "light");
    CHECK(fresh.is_new_install());
    fv::SettingsStore restarted(path);
    CHECK_FALSE(restarted.is_new_install());
}

TEST_CASE("the file signature changes with every save and outside edit") {
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    fv::SettingsStore store(path);
    const auto missing = store.file_signature();
    REQUIRE(missing);
    CHECK_FALSE(missing->exists);
    CHECK(missing->path.is_absolute());
    store.save_setting("a", 1);
    const auto first = store.file_signature();
    REQUIRE(first);
    CHECK(first->exists);
    CHECK(first != missing);
    store.save_setting("a", 2);
    CHECK(store.file_signature() != first);
}

TEST_CASE("watchers hear the keys a save changed, in watch order") {
    fv::MemorySettingsStore store(Json{{"a", 1}, {"b", "x"}});
    InlineExecutor inline_executor;
    std::vector<Json> heard;
    auto watch = store.watch({"c", "b", "a", "b"}, inline_executor,
                             [&](const Json& changed) { heard.push_back(changed); });
    store.update(Json{{"a", 2}, {"b", "x"}, {"c", true}});
    REQUIRE(heard.size() == 1);
    CHECK(heard[0] == Json::array({"c", "a"}));
    // Python ==: 2 == 2.0, so nothing changed.
    store.update(Json{{"a", 2.0}});
    CHECK(heard.size() == 1);
    store.update(Json::object(), {"c"});
    REQUIRE(heard.size() == 2);
    CHECK(heard[1] == Json::array({"c"}));
    store.save_all(Json{{"a", 2.0}, {"b", "y"}});
    REQUIRE(heard.size() == 3);
    CHECK(heard[2] == Json::array({"b"}));
    // An outside edit is not a save through the store.
    store.set_contents(Json{{"a", 5}});
    CHECK(store.snapshot()->at("a") == 5);
    CHECK(heard.size() == 3);
    watch.disconnect();
    store.update(Json{{"a", 6}});
    CHECK(heard.size() == 3);
}

TEST_CASE("a watcher on another executor is posted, and one that throws is logged") {
    fv::MemorySettingsStore store;
    fv::ManualExecutor elsewhere;
    InlineExecutor here;
    std::vector<Json> heard;
    auto posted =
        store.watch({"a"}, elsewhere, [&](const Json& changed) { heard.push_back(changed); });
    auto failing = store.watch({"a"}, here, [](const Json&) { throw std::runtime_error("boom"); });
    LogCapture log;
    store.update(Json{{"a", 1}});
    CHECK(log.contains(fv::log::Level::Error, "A settings watcher failed"));
    CHECK(heard.empty());
    elsewhere.run_all();
    REQUIRE(heard.size() == 1);
    CHECK(heard[0] == Json::array({"a"}));
}

TEST_CASE("a failed mutate tells no watcher") {
    fv::MemorySettingsStore store;
    InlineExecutor here;
    int calls = 0;
    auto watch = store.watch({"a"}, here, [&](const Json&) { ++calls; });
    CHECK_THROWS(store.mutate([](Json& settings) {
        settings["a"] = 1;
        throw std::runtime_error("no");
    }));
    store.file().fail_next_replace("blocked");
    CHECK_THROWS(store.update(Json{{"a", 1}}));
    CHECK(calls == 0);
    CHECK(store.contents() == Json::object());
}

TEST_CASE("the memory store behaves as the file store does") {
    fv::MemorySettingsStore store;
    CHECK_FALSE(store.is_new_install());
    CHECK(store.contents() == Json::object());
    store.save_setting("ui_theme", "light");
    CHECK(store.file().bytes() == with_os_linesep("{\n  \"ui_theme\": \"light\"\n}\n"));
    // The save dropped the cache: one read, then the cached mapping until the file changes.
    const auto reads = store.file().reads();
    CHECK(store.get(fv::keys::ui_theme) == "light");
    for (int i = 0; i < 3; ++i) {
        store.snapshot();
    }
    CHECK(store.file().reads() == reads + 1);
    store.set_contents(Json{{"ui_theme", "system"}});
    CHECK(store.get(fv::keys::ui_theme) == "system");

    // A named mapping, const or not, is a file holding it (GCC once found this ambiguous).
    const Json initial{{"a", 1}};
    fv::MemorySettingsStore from_lvalue(initial);
    CHECK(from_lvalue.contents() == initial);
    CHECK_FALSE(from_lvalue.is_new_install());

    fv::MemorySettingsStore fresh(std::nullopt);
    CHECK(fresh.is_new_install());
    CHECK(*fresh.snapshot() == Json::object());
    CHECK_FALSE(fresh.file().bytes().has_value());
    fresh.file().set_bytes("{oops");
    CHECK(*fresh.snapshot() == Json::object());
    CHECK_THROWS_AS(fresh.update(Json{{"a", 1}}), fv::SettingsFileError);
}

TEST_CASE("update and save_all refuse a mapping that is not an object") {
    fv::MemorySettingsStore store;
    CHECK_THROWS_AS(store.update(Json::array()), std::invalid_argument);
    CHECK_THROWS_AS(store.save_all(Json("x")), std::invalid_argument);
    CHECK_THROWS_AS(store.mutate([](Json& settings) { settings = Json::array(); }),
                    std::invalid_argument);
    CHECK(store.contents() == Json::object());
}
