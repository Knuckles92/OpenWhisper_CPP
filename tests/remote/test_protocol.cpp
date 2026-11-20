// remote/protocol.h and remote/wire.h against services/remote_asr/protocol.py: the ported tests
// of tests/test_remote_engine.py, test_remote_records.py, test_personalize_s4_remote.py and
// test_remote_pairing_ux.py, then the golden vectors
// (tests/golden/services.remote_asr.protocol.json).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "base/hash.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_test_support.h"

using namespace fv;
using namespace fv::remote;
using fv::remote::test::bytes_from_hex;
using fv::remote::test::bytes_of;
using fv::remote::test::from_hex;
using fv::remote::test::hex_of;

namespace {

constexpr std::string_view kModule = "services.remote_asr.protocol";

std::vector<std::uint8_t> repeated(char ch, std::size_t count) {
    return std::vector<std::uint8_t>(count, static_cast<std::uint8_t>(ch));
}

// The wrapper's _catching record of a call: {"result": ...} or {"error", "message"}.
template <class Fn> Json catching(Fn&& fn) {
    try {
        return Json{{"result", fn()}};
    } catch (const py::PyError& error) {
        return Json{{"error", std::string(error.type_name())}, {"message", error.what()}};
    }
}

Json floats(const std::vector<float>& values) {
    Json out = Json::array();
    for (const float value : values) {
        out.push_back(static_cast<double>(value));
    }
    return out;
}

} // namespace

// ---- tests/test_remote_engine.py ----

TEST_CASE("request frames round trip int16 audio exactly", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_request_frames_round_trip_int16_audio_exactly
    const std::vector<std::int16_t> pcm = {-32768, -1, 0, 1, 12345, 32767};
    std::vector<float> audio;
    for (const auto sample : pcm) {
        audio.push_back(static_cast<float>(sample) / 32768.0f);
    }
    const Json header = {{"id", 7}, {"op", "transcribe"}, {"language", "en"}};
    const auto [unpacked, decoded] = unpack_request(pack_request(header, audio));
    CHECK((unpacked == header));
    CHECK(decoded == audio);
}

TEST_CASE("request frames reject garbage", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_request_frames_reject_garbage
    CHECK_THROWS_AS(unpack_request(bytes_from_hex("00")), ProtocolError);
    CHECK_THROWS_AS(unpack_request(bytes_of(std::string("\x00\x00\x00\x09nope", 8))),
                    ProtocolError);
    CHECK_THROWS_AS(unpack_request(bytes_of("text frame"), false), ProtocolError);
}

TEST_CASE("parse address", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_parse_address
    const auto [text, host, port] = GENERATE(table<std::string, std::string, int>({
        {"devbox", "devbox", DEFAULT_PORT},
        {"192.168.1.20:5000", "192.168.1.20", 5000},
        {"[fe80::1]:6000", "fe80::1", 6000},
        {"fe80::1", "fe80::1", DEFAULT_PORT},
        {"https://devbox.local:7000/whatever", "devbox.local", 7000},
    }));
    CHECK(parse_address(text) == std::pair(host, port));
}

TEST_CASE("parse address rejects bad input", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_parse_address_rejects_bad_input
    const auto text = GENERATE(as<std::string>{}, "", "   ", "host:notaport", "host:0", "[fe80::1");
    CHECK_THROWS_AS(parse_address(text), py::ValueError);
}

TEST_CASE("short fingerprint is five groups", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_short_fingerprint_is_five_groups
    std::string fingerprint;
    for (int i = 0; i < 32; ++i) {
        fingerprint += "ab";
    }
    CHECK(short_fingerprint(fingerprint) == "ABAB-ABAB-ABAB-ABAB-ABAB");
}

// ---- tests/test_remote_records.py ----

TEST_CASE("payload frames carry raw bytes and audio frames still decode", "[test_remote_records]") {
    // from
    // tests/test_remote_records.py::test_payload_frames_carry_raw_bytes_and_audio_frames_still_decode
    const std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    const Frame frame = unpack_frame(pack_request({{"op", "records_put"}}, payload));
    CHECK((frame.header == Json{{"op", "records_put"}}));
    CHECK(frame.payload == payload);
    const std::vector<float> audio = {0.0f, 0.5f, -0.5f};
    const auto [header, decoded] = unpack_request(pack_request({{"op", "transcribe"}}, audio));
    REQUIRE(decoded.size() == audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) {
        CHECK(std::fabs(decoded[i] - audio[i]) <= 1.0 / 32768);
    }
}

// ---- tests/test_personalize_s4_remote.py::TestHeaderPhrases ----

TEST_CASE("valid phrases pass", "[test_personalize_s4_remote]") {
    // from tests/test_personalize_s4_remote.py::TestHeaderPhrases::test_valid_phrases_pass
    const Json header = {{"phrases", {"Ksenia", "  Kuber   netes "}}};
    CHECK(header_phrases(header) == std::vector<std::string>{"Ksenia", "Kuber netes"});
}

TEST_CASE("malformed values give nothing", "[test_personalize_s4_remote]") {
    // from
    // tests/test_personalize_s4_remote.py::TestHeaderPhrases::test_malformed_values_give_nothing
    const Json value = GENERATE(Json(nullptr), Json("Ksenia"), Json{{"a", 1}}, Json(5),
                                Json::array(), Json::array({nullptr, 3, "", "  "}));
    CHECK(header_phrases(Json{{"phrases", value}}).empty());
    CHECK(header_phrases(Json::object()).empty());
}

TEST_CASE("limits", "[test_personalize_s4_remote]") {
    // from tests/test_personalize_s4_remote.py::TestHeaderPhrases::test_limits
    Json phrases = Json::array({std::string(65, 'x')});
    for (int i = 0; i < 60; ++i) {
        phrases.push_back("w" + std::to_string(i));
    }
    const auto kept = header_phrases(Json{{"phrases", phrases}});
    CHECK(kept.size() == MAX_HINT_PHRASES);
    CHECK(kept.front() == "w0");
}

TEST_CASE("a full header still fits the frame limit", "[test_personalize_s4_remote]") {
    // from
    // tests/test_personalize_s4_remote.py::TestHeaderPhrases::test_a_full_header_still_fits_the_frame_limit
    std::string phrase;
    for (std::size_t i = 0; i < MAX_HINT_CHARS; ++i) {
        phrase += "\xC3\xA9";
    }
    const Json phrases = Json(std::vector<std::string>(MAX_HINT_PHRASES, phrase));
    const Json header = {{"id", 1}, {"op", "transcribe"}, {"language", "en"}, {"phrases", phrases}};
    const std::vector<float> silence(16, 0.0f);
    CHECK((unpack_frame(pack_request(header, silence)).header["phrases"] == phrases));
}

// ---- tests/test_remote_pairing_ux.py ----

TEST_CASE("both sides get the same number only through the same certificate",
          "[test_remote_pairing_ux]") {
    // from
    // tests/test_remote_pairing_ux.py::test_both_sides_get_the_same_number_only_through_the_same_certificate
    const auto host_nonce = repeated('h', 32);
    const auto client_nonce = repeated('c', 32);
    std::string upper, lower, other;
    for (int i = 0; i < 32; ++i) {
        upper += "AB";
        lower += "ab";
        other += "CD";
    }
    const std::string sas = pairing_sas(host_nonce, client_nonce, upper);
    CHECK(sas.size() == 6);
    CHECK(sas.find_first_not_of("0123456789") == std::string::npos);
    CHECK(sas == pairing_sas(host_nonce, client_nonce, lower));
    // A relay in the middle presents its own certificate: the numbers differ.
    CHECK(sas != pairing_sas(host_nonce, client_nonce, other));
    CHECK(sas != pairing_sas(host_nonce, repeated('d', 32), upper));
    CHECK(format_sas("123456") == "123 456");
    CHECK(pairing_commitment(host_nonce) != pairing_commitment(repeated('x', 32)));
}

// ---- what the vectors cannot hold ----

TEST_CASE("a header keeps a lone surrogate it was sent in UTF-8", "[protocol]") {
    // json.loads of bytes decodes with "surrogatepass", so ED A0 80 is U+D800, not an error.
    const std::string body = "{\"a\":\"\xED\xA0\x80\"}";
    std::vector<std::byte> frame = {std::byte{0}, std::byte{0}, std::byte{0},
                                    static_cast<std::byte>(body.size())};
    const auto bytes = bytes_of(body);
    frame.insert(frame.end(), bytes.begin(), bytes.end());
    CHECK(unpack_frame(frame).header["a"].get<std::string>() == "\xED\xA0\x80");
}

TEST_CASE("the two serializers differ only in their separators", "[protocol]") {
    const Json value = {{"type", "hello"}, {"protocol", 1}, {"name", "Zo\xC3\xAB"}, {"x", 0.5}};
    CHECK(dumps_compact(value) == R"({"type":"hello","protocol":1,"name":"Zo\u00eb","x":0.5})");
    CHECK(dumps_default(value) ==
          R"({"type": "hello", "protocol": 1, "name": "Zo\u00eb", "x": 0.5})");
}

TEST_CASE("NaN samples encode as silence", "[protocol]") {
    const std::vector<float> samples = {std::nanf("")};
    CHECK(hex_of(encode_audio(samples)) == "0000");
}

TEST_CASE("the wire constants are OpenWhisper's", "[protocol]") {
    CHECK(PATH == "/openwhisper/v1/engine");
    CHECK(NOT_AN_ENDPOINT_BODY == "Not an OpenWhisper remote engine endpoint\n");
    CHECK(MAX_REQUEST_BYTES == 3'905'540);
    CHECK(QUERY_MAGIC == "OWHISPER?1");
    CHECK(REPLY_MAGIC == "OWHISPER!1");
}

// ---- golden vectors ----

TEST_CASE("protocol constants match Python", "[golden]") {
    golden::for_each(kModule, "constants", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(expected["PROTOCOL_VERSION"] == PROTOCOL_VERSION);
        CHECK(expected["DEFAULT_PORT"] == DEFAULT_PORT);
        CHECK(expected["PATH"].get<std::string>() == PATH);
        CHECK(expected["SAMPLE_RATE"] == SAMPLE_RATE);
        CHECK(expected["MAX_AUDIO_SECONDS"] == MAX_AUDIO_SECONDS);
        CHECK(expected["MAX_HEADER_BYTES"] == MAX_HEADER_BYTES);
        CHECK(expected["MAX_REQUEST_BYTES"] == MAX_REQUEST_BYTES);
        CHECK(expected["MAX_REPLY_BYTES"] == MAX_REPLY_BYTES);
        CHECK(expected["RECORD_CHUNK_BYTES"] == RECORD_CHUNK_BYTES);
        CHECK(expected["CLOSE_UNAUTHORIZED"] == CLOSE_UNAUTHORIZED);
        CHECK(expected["CLOSE_BAD_REQUEST"] == CLOSE_BAD_REQUEST);
        CHECK(expected["CLOSE_TIMEOUT"] == CLOSE_TIMEOUT);
        CHECK(expected["CLOSE_ENGINE_CHANGED"] == CLOSE_ENGINE_CHANGED);
        CHECK(expected["ENGINE_RELOADING"].get<std::string>() == ENGINE_RELOADING);
        CHECK(expected["CLOSE_BUSY"] == CLOSE_BUSY);
        CHECK(expected["MAX_NAME"] == MAX_NAME);
        CHECK(expected["MAX_HINT_PHRASES"] == MAX_HINT_PHRASES);
        CHECK(expected["MAX_HINT_CHARS"] == MAX_HINT_CHARS);
        CHECK(expected["PAIRING_NONCE_BYTES"] == PAIRING_NONCE_BYTES);
        CHECK(expected["SAS_DIGITS"] == SAS_DIGITS);
    });
}

TEST_CASE("encode_audio matches Python", "[golden]") {
    golden::for_each(kModule, "encode_audio", [](const golden::Case& c) {
        std::vector<float> samples;
        for (const double value : c.arg<std::vector<double>>(0)) {
            samples.push_back(static_cast<float>(value));
        }
        CHECK(hex_of(encode_audio(samples)) == c.result<std::string>());
    });
}

TEST_CASE("decode_audio matches Python", "[golden]") {
    golden::for_each(kModule, "decode_audio", [](const golden::Case& c) {
        const auto data = bytes_from_hex(c.arg<std::string>(0));
        CHECK(
            golden::equal(c.result<Json>(), catching([&] { return floats(decode_audio(data)); })));
    });
}

TEST_CASE("pack_request matches Python byte for byte", "[golden]") {
    golden::for_each(kModule, "pack_request", [](const golden::Case& c) {
        const Json header = c.arg<Json>(0);
        const Json actual = catching([&] {
            if (c.arg_count() >= 3 && !c.args()[2].is_null()) {
                return hex_of(pack_request(header, bytes_from_hex(c.arg<std::string>(2))));
            }
            if (c.arg_count() >= 2 && !c.args()[1].is_null()) {
                std::vector<float> audio;
                for (const double value : c.arg<std::vector<double>>(1)) {
                    audio.push_back(static_cast<float>(value));
                }
                return hex_of(pack_request(header, audio));
            }
            return hex_of(pack_request(header));
        });
        CHECK((actual == c.result<Json>()));
    });
}

TEST_CASE("pack_request refuses headers past 64 KiB as Python does", "[golden]") {
    golden::for_each(kModule, "pack_request_sized", [](const golden::Case& c) {
        std::string phrase;
        const std::string unit = c.arg<std::string>(0);
        for (int i = 0; i < c.arg<int>(1); ++i) {
            phrase += unit;
        }
        const Json actual = catching([&] {
            const auto frame = pack_request(Json{{"phrases", {phrase}}});
            Sha256 hash;
            hash.update(frame);
            return Json::array({frame.size(), hash.hexdigest()});
        });
        CHECK((actual == c.result<Json>()));
    });
}

TEST_CASE("unpack_frame matches Python", "[golden]") {
    golden::for_each(kModule, "unpack_frame", [](const golden::Case& c) {
        const bool binary = c.arg_count() < 2 || c.arg<bool>(1);
        const auto data =
            binary ? bytes_from_hex(c.arg<std::string>(0)) : bytes_of(c.arg<std::string>(0));
        const Json actual = catching([&] {
            Frame frame = unpack_frame(data, binary);
            return Json::array({frame.header, hex_of(frame.payload)});
        });
        INFO(actual.dump());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("unpack_frame's header length limit matches Python", "[golden]") {
    golden::for_each(kModule, "unpack_frame_sized", [](const golden::Case& c) {
        const auto length = c.arg<std::uint32_t>(0);
        std::vector<std::byte> frame;
        for (int shift = 24; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<std::byte>((length >> shift) & 0xFF));
        }
        frame.push_back(std::byte{'{'});
        frame.insert(frame.end(), static_cast<std::size_t>(c.arg<int>(1)), std::byte{' '});
        frame.push_back(std::byte{'}'});
        frame.insert(frame.end(), static_cast<std::size_t>(c.arg<int>(2)), std::byte{0});
        const Json actual = catching([&] {
            Frame unpacked = unpack_frame(frame);
            return Json::array({unpacked.header, unpacked.payload.size()});
        });
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("payload_audio matches Python", "[golden]") {
    golden::for_each(kModule, "payload_audio", [](const golden::Case& c) {
        const auto data = bytes_from_hex(c.arg<std::string>(0));
        CHECK(
            golden::equal(c.result<Json>(), catching([&] { return floats(payload_audio(data)); })));
    });
    golden::for_each(kModule, "payload_audio_size", [](const golden::Case& c) {
        const std::vector<std::byte> data(c.arg<std::size_t>(0), std::byte{0});
        CHECK(
            golden::equal(c.result<Json>(), catching([&] { return payload_audio(data).size(); })));
    });
}

TEST_CASE("header_phrases matches Python", "[golden]") {
    golden::for_each(kModule, "header_phrases", [](const golden::Case& c) {
        CHECK((Json(header_phrases(c.arg<Json>(0))) == c.result<Json>()));
    });
}

TEST_CASE("fingerprints, digests and names match Python", "[golden]") {
    golden::for_each(kModule, "certificate_fingerprint", [](const golden::Case& c) {
        CHECK(certificate_fingerprint(bytes_from_hex(c.arg<std::string>(0))) ==
              c.result<std::string>());
    });
    golden::for_each(kModule, "short_fingerprint", [](const golden::Case& c) {
        const int groups = c.arg_count() > 1 ? c.arg<int>(1) : 5;
        CHECK(short_fingerprint(c.arg<std::string>(0), groups) == c.result<std::string>());
    });
    golden::for_each(kModule, "token_digest", [](const golden::Case& c) {
        CHECK(token_digest(c.arg<std::string>(0)) == c.result<std::string>());
    });
    golden::for_each(kModule, "clean_name", [](const golden::Case& c) {
        CHECK(clean_name(c.arg<Json>(0)) == c.result<std::string>());
    });
}

TEST_CASE("pairing numbers match Python", "[golden]") {
    golden::for_each(kModule, "pairing_commitment", [](const golden::Case& c) {
        CHECK(pairing_commitment(from_hex(c.arg<std::string>(0))) == c.result<std::string>());
    });
    golden::for_each(kModule, "pairing_sas", [](const golden::Case& c) {
        CHECK(pairing_sas(from_hex(c.arg<std::string>(0)), from_hex(c.arg<std::string>(1)),
                          c.arg<std::string>(2)) == c.result<std::string>());
    });
    golden::for_each(kModule, "format_sas", [](const golden::Case& c) {
        CHECK(format_sas(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("addresses match Python", "[golden]") {
    golden::for_each(kModule, "parse_address", [](const golden::Case& c) {
        const int default_port = c.arg_count() > 1 ? c.arg<int>(1) : DEFAULT_PORT;
        const Json actual = catching([&] {
            const auto [host, port] = parse_address(c.arg<std::string>(0), default_port);
            return Json::array({host, port});
        });
        CHECK((actual == c.result<Json>()));
    });
    golden::for_each(kModule, "format_address", [](const golden::Case& c) {
        CHECK(format_address(c.arg<std::string>(0), c.arg<int>(1)) == c.result<std::string>());
    });
}
