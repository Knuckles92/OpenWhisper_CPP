// The microphone rules of ui_qt/dialogs/settings_microphones.py: the golden vectors of
// ui_qt.dialogs.settings_microphones (over M1.3's dumped device lists) and the reorder rules and
// labels.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "appcore/mic_priority.h"
#include "audio/devices.h"
#include "base/json.h"
#include "golden/golden.h"

using fv::Json;
namespace mic = fv::appcore::mic;
using mic::DeviceKey;
using mic::Devices;
using mic::MicDevice;

namespace {

constexpr const char* kModule = "ui_qt.dialogs.settings_microphones";

MicDevice device_from(const Json& dict) {
    return MicDevice{dict.at("index").get<int>(),           dict.at("name").get<std::string>(),
                     dict.at("hostapi").get<std::string>(), dict.at("default_api").get<bool>(),
                     dict.at("system_alias").get<bool>(),   dict.at("label").get<std::string>()};
}

// The device lists the vectors name, as the "devices" function recorded them.
const std::map<std::string, Devices>& device_sets() {
    static const std::map<std::string, Devices> sets = [] {
        // Read from the document, not with golden::for_each: each of its cases is a Catch2
        // section, and only one runs per pass of a test case.
        std::map<std::string, Devices> made;
        for (const auto& c : fv::golden::load(kModule).at("functions").at("devices")) {
            const std::string name = c.at("args")[0].get<std::string>();
            const Json& listed = c.at("result");
            if (listed.is_null()) {
                made[name] = std::nullopt;
                continue;
            }
            std::vector<MicDevice> devices;
            for (const auto& dict : listed) {
                devices.push_back(device_from(dict));
            }
            made[name] = std::move(devices);
        }
        return made;
    }();
    return sets;
}

const Devices& devices_named(const std::string& name) {
    return device_sets().at(name);
}

std::vector<DeviceKey> keys_from(const Json& list) {
    return fv::audio::normalize_priority(list);
}

Json keys_json(const std::vector<DeviceKey>& keys) {
    return fv::audio::priority_json(keys);
}

Json items_json(const std::vector<mic::ComboItem>& items) {
    Json out = Json::array();
    for (const auto& item : items) {
        out.push_back(Json::array({item.text, item.token ? Json(*item.token) : Json(nullptr)}));
    }
    return out;
}

DeviceKey key(std::string name, std::string hostapi) {
    return DeviceKey{std::move(name), std::move(hostapi)};
}

MicDevice listed_device(int index, std::string name, std::string hostapi = "MME") {
    return MicDevice{index, std::move(name), std::move(hostapi), true, false, ""};
}

} // namespace

TEST_CASE("microphone tokens and summaries replay the golden vectors", "[golden][mic_priority]") {
    REQUIRE(device_sets().size() == 4);
    fv::golden::for_each(kModule, "entry_token", [](const fv::golden::Case& c) {
        const Json entry = c.arg<Json>(0);
        CHECK(mic::entry_token(key(entry.at("name").get<std::string>(),
                                   entry.at("hostapi").get<std::string>())) ==
              c.result<std::string>());
    });
    fv::golden::for_each(kModule, "token_entry", [](const fv::golden::Case& c) {
        const Json token = c.arg<Json>(0);
        const Json expected = c.result<Json>();
        // A None token is the Add combo's placeholder: the section never parses it.
        if (token.is_null()) {
            CHECK(expected.is_null());
            return;
        }
        const auto entry = mic::token_entry(token.get<std::string>());
        REQUIRE(entry.has_value() == !expected.is_null());
        if (entry) {
            CHECK(entry->name == expected.at("name").get<std::string>());
            CHECK(entry->hostapi == expected.at("hostapi").get<std::string>());
        }
    });
    fv::golden::for_each(kModule, "saved_priority", [](const fv::golden::Case& c) {
        CHECK(fv::golden::equal(c.result<Json>(), keys_json(mic::saved_priority(c.arg<Json>(0)))));
    });
    fv::golden::for_each(kModule, "preferred_token", [](const fv::golden::Case& c) {
        CHECK(mic::preferred_token(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "rail_value", [](const fv::golden::Case& c) {
        CHECK(mic::rail_value(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "preferred_name", [](const fv::golden::Case& c) {
        CHECK(mic::preferred_name(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "_listed", [](const fv::golden::Case& c) {
        const auto& devices = *devices_named(c.arg<std::string>(0));
        std::vector<bool> listed;
        for (const auto& device : devices) {
            listed.push_back(mic::listed(device));
        }
        CHECK(listed == c.result<std::vector<bool>>());
    });
}

TEST_CASE("microphone section rules replay the golden vectors", "[golden][mic_priority]") {
    fv::golden::for_each(kModule, "MicrophoneSection._label", [](const fv::golden::Case& c) {
        const Json entry = c.arg<Json>(0);
        const auto [label, connected] = mic::entry_label(
            key(entry.at("name").get<std::string>(), entry.at("hostapi").get<std::string>()),
            devices_named(c.arg<std::string>(1)));
        const Json expected = c.result<Json>();
        CHECK(label == expected[0].get<std::string>());
        CHECK(connected == expected[1].get<bool>());
    });
    fv::golden::for_each(
        kModule, "MicrophoneSection._fill_preferred", [](const fv::golden::Case& c) {
            const auto combo = mic::preferred_combo(keys_from(c.arg<Json>(0)),
                                                    devices_named(c.arg<std::string>(1)));
            const Json expected = c.result<Json>();
            CHECK(fv::golden::equal(expected.at("items"), items_json(combo.items)));
            CHECK(combo.current == expected.at("current").get<std::size_t>());
        });
    fv::golden::for_each(
        kModule, "MicrophoneSection._fill_add_combo", [](const fv::golden::Case& c) {
            const auto combo =
                mic::add_combo(keys_from(c.arg<Json>(0)), devices_named(c.arg<std::string>(1)));
            const Json expected = c.result<Json>();
            CHECK(fv::golden::equal(expected.at("items"), items_json(combo.items)));
            CHECK(combo.enabled == expected.at("enabled").get<bool>());
            CHECK(expected.at("current").get<int>() == 0);
        });
    fv::golden::for_each(
        kModule, "MicrophoneSection.choose_preferred", [](const fv::golden::Case& c) {
            const auto save =
                mic::choose_preferred(c.arg<std::string>(0), keys_from(c.arg<Json>(1)),
                                      devices_named(c.arg<std::string>(2)));
            const Json expected = c.result<Json>();
            REQUIRE(save.has_value() == !expected.is_null());
            if (!save) {
                return;
            }
            CHECK(fv::golden::equal(expected.at("priority"), keys_json(save->priority)));
            CHECK(fv::golden::equal(expected.at("updates"), save->updates));
            CHECK(save->updates.dump() == expected.at("updates").dump());
            CHECK(save->drops == expected.at("drops").get<std::vector<std::string>>());
        });
    fv::golden::for_each(kModule, "MicrophoneSection._move", [](const fv::golden::Case& c) {
        const auto moved =
            mic::move(keys_from(c.arg<Json>(0)), c.arg<std::size_t>(1), c.arg<int>(2));
        const Json expected = c.result<Json>();
        REQUIRE(moved.has_value() == !expected.is_null());
        if (moved) {
            CHECK(fv::golden::equal(expected, keys_json(*moved)));
        }
    });
    fv::golden::for_each(kModule, "MicrophoneSection._remove", [](const fv::golden::Case& c) {
        const auto kept = mic::remove(keys_from(c.arg<Json>(0)), c.arg<std::size_t>(1));
        const Json expected = c.result<Json>();
        REQUIRE(kept.has_value() == !expected.is_null());
        if (kept) {
            CHECK(fv::golden::equal(expected, keys_json(*kept)));
        }
    });
    fv::golden::for_each(
        kModule, "MicrophoneSection._on_add_chosen", [](const fv::golden::Case& c) {
            const Json token = c.arg<Json>(1);
            const Json expected = c.result<Json>();
            CHECK(expected.at("reset_to").get<int>() == 0);
            std::optional<std::vector<DeviceKey>> added;
            if (token.is_string()) {
                added = mic::add(keys_from(c.arg<Json>(0)), token.get<std::string>());
            }
            REQUIRE(added.has_value() == !expected.at("saved").is_null());
            if (added) {
                CHECK(fv::golden::equal(expected.at("saved"), keys_json(*added)));
            }
        });
}

TEST_CASE("the rail names the first two microphones and counts the rest", "[mic_priority]") {
    CHECK(mic::rail_value(Json::object()) == "System default");
    const Json three = {{"audio_input_priority",
                         {{{"name", "Microphone (Blue Snowball)"}, {"hostapi", "MME"}},
                          {{"name", "Headset (Jabra)"}, {"hostapi", "MME"}},
                          {{"name", "Microphone (USB)"}, {"hostapi", "MME"}}}}};
    CHECK(mic::rail_value(three) == "Blue Snowball → Jabra +1");
    CHECK(mic::preferred_name(three) == "Blue Snowball");
    CHECK(mic::preferred_name(Json::object()) == "Default microphone");
}

TEST_CASE("a saved microphone is called Not connected only after the device list arrived",
          "[mic_priority]") {
    const std::vector<DeviceKey> priority{key("Microphone (Blue Snowball)", "MME"),
                                          key("Microphone (Rode NT-USB)", "MME")};
    SECTION("before the first list") {
        const Devices unknown;
        const auto rows = mic::backup_rows(priority, unknown);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].label == "Microphone (Rode NT-USB)");
        CHECK(rows[0].note.empty());
        CHECK(rows[0].state.empty());
        const auto combo = mic::preferred_combo(priority, unknown);
        REQUIRE(combo.items.size() == 3);
        CHECK(combo.items[1].text == "Microphone (Blue Snowball)");
        CHECK(combo.items[2].text == "Microphone (Rode NT-USB)");
        CHECK(combo.current == 1);
        const auto add = mic::add_combo(priority, unknown);
        REQUIRE(add.items.size() == 1);
        CHECK(add.items[0].text == "Looking for microphones…");
        CHECK_FALSE(add.enabled);
    }
    SECTION("once listed") {
        const Devices listed =
            std::vector<MicDevice>{listed_device(1, "Microphone (Blue Snowball)"),
                                   listed_device(2, "Headset (Jabra Evolve2)")};
        const auto rows = mic::backup_rows(priority, listed);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].note == "Not connected");
        CHECK(rows[0].state == "missing");
        CHECK(rows[0].index == 1);
        CHECK_FALSE(rows[0].up);
        CHECK_FALSE(rows[0].down);
        const auto combo = mic::preferred_combo(priority, listed);
        REQUIRE(combo.items.size() == 4);
        CHECK(combo.items[3].text == "Microphone (Rode NT-USB) · Not connected");
        const auto add = mic::add_combo(priority, listed);
        REQUIRE(add.items.size() == 2);
        CHECK(add.items[0].text == "Add a microphone…");
        CHECK(add.items[1].text == "Headset (Jabra Evolve2)");
        CHECK(add.enabled);
    }
}

TEST_CASE("backups move only among themselves", "[mic_priority]") {
    const std::vector<DeviceKey> priority{key("A", "MME"), key("B", "MME"), key("C", "MME")};
    CHECK_FALSE(mic::move(priority, 0, 1));
    CHECK_FALSE(mic::move(priority, 1, -1));
    CHECK_FALSE(mic::move(priority, 2, 1));
    CHECK(*mic::move(priority, 1, 1) ==
          std::vector<DeviceKey>{key("A", "MME"), key("C", "MME"), key("B", "MME")});
    CHECK(*mic::move(priority, 2, -1) ==
          std::vector<DeviceKey>{key("A", "MME"), key("C", "MME"), key("B", "MME")});
    CHECK_FALSE(mic::remove(priority, 0));
    CHECK(*mic::remove(priority, 2) == std::vector<DeviceKey>{key("A", "MME"), key("B", "MME")});

    const auto rows = mic::backup_rows(priority, std::nullopt);
    REQUIRE(rows.size() == 2);
    CHECK_FALSE(rows[0].up);
    CHECK(rows[0].down);
    CHECK(rows[1].up);
    CHECK_FALSE(rows[1].down);
}

TEST_CASE("choosing a microphone puts it first and keeps the legacy index where it can open",
          "[mic_priority]") {
    const std::vector<DeviceKey> priority{key("Microphone (A)", "MME"),
                                          key("Microphone (B)", "MME")};
    const Devices listed = std::vector<MicDevice>{
        listed_device(1, "Microphone (A)"), listed_device(4, "Microphone (B)"),
        MicDevice{7, "Microphone (C)", "Windows WASAPI", false, false, ""}};

    const auto chosen =
        mic::choose_preferred(mic::entry_token(key("Microphone (B)", "MME")), priority, listed);
    REQUIRE(chosen);
    CHECK(chosen->priority ==
          std::vector<DeviceKey>{key("Microphone (B)", "MME"), key("Microphone (A)", "MME")});
    CHECK(chosen->updates.at("audio_input_device") == 4);
    CHECK(chosen->drops.empty());

    const auto wasapi = mic::choose_preferred(
        mic::entry_token(key("Microphone (C)", "Windows WASAPI")), priority, listed);
    REQUIRE(wasapi);
    CHECK_FALSE(wasapi->updates.contains("audio_input_device"));
    CHECK(wasapi->drops == std::vector<std::string>{"audio_input_device"});

    const auto system = mic::choose_preferred(mic::kSystemDefault, priority, listed);
    REQUIRE(system);
    CHECK(system->priority.empty());
    CHECK(system->updates.at("audio_input_priority") == Json::array());

    CHECK_FALSE(mic::choose_preferred("not json", priority, listed));
    CHECK_FALSE(mic::add({}, mic::entry_token(key("Microphone (A)", "MME"))));
    CHECK_FALSE(mic::add(priority, mic::entry_token(key("Microphone (A)", "MME"))));
    CHECK(mic::add(priority, mic::entry_token(key("Microphone (C)", "Windows WASAPI")))->size() ==
          3);
}

TEST_CASE("a listed input maps onto the rules' device", "[mic_priority]") {
    fv::audio::InputDevice input;
    input.index = 3;
    input.name = "Microphone (2- USB Audio Device";
    input.hostapi = "MME";
    input.label = "Microphone (2- USB Audio Device)";
    input.default_api = true;
    const MicDevice device = mic::mic_device(input);
    CHECK(device.index == 3);
    CHECK(device.display() == input.display());
    CHECK_FALSE(device.system_alias);
    input.name = "Microsoft Sound Mapper - Input";
    CHECK(mic::mic_device(input).system_alias);
    CHECK_FALSE(mic::listed(mic::mic_device(input)));
}
