// One real call into each third-party library of vcpkg.json.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/http.hpp>
#include <curl/curl.h>
#include <minizip-ng/mz.h>
#include <minizip-ng/mz_os.h>
#include <minizip-ng/mz_strm.h>
#include <minizip-ng/mz_zip.h>
#include <minizip-ng/mz_zip_rw.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pcre2.h>
#include <pocketfft_hdronly.h>
#include <portaudio.h>
#include <pugixml.hpp>
#include <sqlite3.h>
#include <zlib.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#ifdef _WIN32
#include <windows.h>

#include <wil/resource.h>
#endif

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

std::string hex(const unsigned char* bytes, std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        char pair[3];
        std::snprintf(pair, sizeof pair, "%02x", bytes[i]);
        out += pair;
    }
    return out;
}

// A fresh folder under the system temporary folder, removed when the test ends.
struct TempFolder {
    std::filesystem::path path;
    TempFolder() {
        std::random_device random;
        path = std::filesystem::temp_directory_path() /
               ("fv_deps_" + std::to_string(random()) + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

} // namespace

TEST_CASE("nlohmann-json ordered_json keeps key order and number kinds through a round trip") {
    const std::string text = R"({"zeta":1,"alpha":2.5,"mid":true,"name":"café","list":[3,1]})";
    const auto parsed = nlohmann::ordered_json::parse(text);
    CHECK(parsed.dump() == text);
    CHECK(parsed["zeta"].is_number_integer());
    CHECK(parsed["alpha"].is_number_float());
    CHECK(parsed.begin().key() == "zeta");
}

TEST_CASE("PCRE2 matches a Unicode \\w+ with UTF and UCP") {
    int error = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>("\\w+"), PCRE2_ZERO_TERMINATED,
                                     PCRE2_UTF | PCRE2_UCP, &error, &error_offset, nullptr);
    REQUIRE(code != nullptr);
    pcre2_match_data* match = pcre2_match_data_create_from_pattern(code, nullptr);

    const std::string subject = "  naïve Straße";
    std::vector<std::string> words;
    PCRE2_SIZE start = 0;
    while (pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), start, 0,
                       match, nullptr) > 0) {
        const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match);
        words.push_back(subject.substr(ovector[0], ovector[1] - ovector[0]));
        start = ovector[1];
    }
    pcre2_match_data_free(match);
    pcre2_code_free(code);
    CHECK(words == std::vector<std::string>{"naïve", "Straße"});
}

TEST_CASE("SQLite has FTS5 and the JSON1 functions") {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    char* message = nullptr;
    const int created = sqlite3_exec(db,
                                     "CREATE VIRTUAL TABLE notes USING fts5(body);"
                                     "INSERT INTO notes(body) VALUES ('the quick brown fox');"
                                     "INSERT INTO notes(body) VALUES ('hello dictation');",
                                     nullptr, nullptr, &message);
    INFO((message ? message : ""));
    sqlite3_free(message);
    REQUIRE(created == SQLITE_OK);

    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT rowid FROM notes WHERE notes MATCH 'dictation'", -1,
                               &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int(statement, 0) == 2);
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);

    REQUIRE(sqlite3_prepare_v2(db, R"(SELECT json_extract('{"a":[1,2,3]}', '$.a[1]'))", -1,
                               &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int(statement, 0) == 2);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

TEST_CASE("libcurl reports its SSL backend, Schannel on Windows") {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    REQUIRE(info != nullptr);
    REQUIRE(info->ssl_version != nullptr);
    const std::string ssl = info->ssl_version;
    std::printf("curl %s, SSL backend: %s\n", info->version, ssl.c_str());
    CHECK((info->features & CURL_VERSION_SSL) != 0);
#ifdef _WIN32
    CHECK_THAT(ssl, ContainsSubstring("Schannel"));
#else
    CHECK_THAT(ssl, ContainsSubstring("OpenSSL"));
#endif
}

TEST_CASE("Boost.Asio runs an io_context and Boost.Beast parses an HTTP request") {
    boost::asio::io_context io;
    bool ran = false;
    boost::asio::post(io, [&] { ran = true; });
    CHECK(io.run() == 1);
    CHECK(ran);

    namespace http = boost::beast::http;
    http::request_parser<http::string_body> parser;
    const std::string_view raw = "GET /api/meetings/7/audio?revision=3 HTTP/1.1\r\n"
                                 "Host: 127.0.0.1:8765\r\n"
                                 "Range: bytes=0-99\r\n"
                                 "\r\n";
    boost::beast::error_code error;
    parser.put(boost::asio::buffer(raw.data(), raw.size()), error);
    REQUIRE_FALSE(error);
    REQUIRE(parser.is_done());
    const auto& request = parser.get();
    CHECK(request.method() == http::verb::get);
    CHECK(request.target() == "/api/meetings/7/audio?revision=3");
    CHECK(request[http::field::host] == "127.0.0.1:8765");
    CHECK(request[http::field::range] == "bytes=0-99");
}

TEST_CASE("OpenSSL 3 reports its version and hashes with SHA-256") {
    CHECK(OpenSSL_version_num() >= 0x30000000L);
    CHECK_THAT(std::string(OpenSSL_version(OPENSSL_VERSION)), StartsWith("OpenSSL 3."));
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    REQUIRE(EVP_Digest("abc", 3, digest, &size, EVP_sha256(), nullptr) == 1);
    CHECK(hex(digest, size) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("PortAudio initialises and lists the host APIs it was built with") {
    REQUIRE(Pa_Initialize() == paNoError);
    std::vector<std::string> names;
    const PaHostApiIndex count = Pa_GetHostApiCount();
    for (PaHostApiIndex i = 0; i < count; ++i) {
        names.emplace_back(Pa_GetHostApiInfo(i)->name);
    }
    Pa_Terminate();
    std::string listed;
    for (const auto& name : names) {
        listed += (listed.empty() ? "" : ", ") + name;
    }
    std::printf("PortAudio %s host APIs: %s\n", Pa_GetVersionText(), listed.c_str());
    INFO("host APIs: " << listed);
    REQUIRE(count >= 0);
#ifndef _WIN32
    // vcpkg's Linux port builds JACK only. It reports zero available host APIs when
    // no JACK server is running, including on a headless CI worker.
    if (count == 0) {
        REQUIRE(Pa_GetVersion() > 0);
        SKIP("PortAudio initialized, but no audio host server is available.");
    }
#endif
    REQUIRE(count >= 1);
#ifdef _WIN32
    // The rows of OpenWhisper's Microphones dialog come from these (sounddevice is PortAudio).
    const auto has = [&](std::string_view name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    CHECK(has("MME"));
    CHECK(has("Windows DirectSound"));
    CHECK(has("Windows WASAPI"));
    CHECK(has("Windows WDM-KS"));
#endif
}

TEST_CASE("FFmpeg is LGPL and decodes the six upload types") {
    CHECK(avformat_version() == LIBAVFORMAT_VERSION_INT);
    CHECK(avcodec_version() == LIBAVCODEC_VERSION_INT);
    CHECK(swresample_version() == LIBSWRESAMPLE_VERSION_INT);
    std::printf("FFmpeg avformat %u.%u.%u, avcodec %u.%u.%u, swresample %u.%u.%u\n",
                AV_VERSION_MAJOR(avformat_version()), AV_VERSION_MINOR(avformat_version()),
                AV_VERSION_MICRO(avformat_version()), AV_VERSION_MAJOR(avcodec_version()),
                AV_VERSION_MINOR(avcodec_version()), AV_VERSION_MICRO(avcodec_version()),
                AV_VERSION_MAJOR(swresample_version()), AV_VERSION_MINOR(swresample_version()),
                AV_VERSION_MICRO(swresample_version()));

    for (const char* configuration :
         {avformat_configuration(), avcodec_configuration(), swresample_configuration()}) {
        const std::string text = configuration;
        INFO(text);
        CHECK_FALSE(text.find("--enable-gpl") != std::string::npos);
        CHECK_FALSE(text.find("--enable-nonfree") != std::string::npos);
    }
    CHECK_THAT(std::string(avformat_license()), StartsWith("LGPL"));
    CHECK_THAT(std::string(avcodec_license()), StartsWith("LGPL"));
    CHECK_THAT(std::string(swresample_license()), StartsWith("LGPL"));

    // WAV, MP3, M4A, OGG, FLAC and WMA, the types the Upload tab accepts.
    for (const char* demuxer : {"wav", "mp3", "mov", "ogg", "flac", "asf"}) {
        INFO(demuxer);
        CHECK(av_find_input_format(demuxer) != nullptr);
    }
    for (const AVCodecID codec : {AV_CODEC_ID_PCM_S16LE, AV_CODEC_ID_PCM_F32LE, AV_CODEC_ID_MP3,
                                  AV_CODEC_ID_AAC, AV_CODEC_ID_VORBIS, AV_CODEC_ID_OPUS,
                                  AV_CODEC_ID_FLAC, AV_CODEC_ID_WMAV1, AV_CODEC_ID_WMAV2}) {
        INFO(avcodec_get_name(codec));
        CHECK(avcodec_find_decoder(codec) != nullptr);
    }
}

TEST_CASE("swresample converts 44.1 kHz s16 mono to 16 kHz") {
    AVChannelLayout mono{};
    av_channel_layout_default(&mono, 1);
    SwrContext* swr = nullptr;
    REQUIRE(swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, 16000, &mono, AV_SAMPLE_FMT_S16,
                                44100, 0, nullptr) == 0);
    REQUIRE(swr_init(swr) == 0);

    std::vector<std::int16_t> input(4410, 0); // 0.1 s
    std::vector<std::int16_t> output(4096);
    const auto* in = reinterpret_cast<const std::uint8_t*>(input.data());
    auto* out = reinterpret_cast<std::uint8_t*>(output.data());
    int produced = swr_convert(swr, &out, static_cast<int>(output.size()), &in,
                               static_cast<int>(input.size()));
    REQUIRE(produced >= 0);
    std::vector<std::int16_t> tail(4096);
    auto* tail_out = reinterpret_cast<std::uint8_t*>(tail.data());
    const int flushed = swr_convert(swr, &tail_out, static_cast<int>(tail.size()), nullptr, 0);
    REQUIRE(flushed >= 0);
    swr_free(&swr);
    CHECK(produced + flushed == 1600);
}

TEST_CASE("pocketfft computes a small real FFT") {
    const std::vector<double> input{1.0, 2.0, 3.0, 4.0};
    std::vector<std::complex<double>> output(3);
    pocketfft::r2c<double>({input.size()}, {sizeof(double)}, {sizeof(std::complex<double>)}, 0,
                           pocketfft::FORWARD, input.data(), output.data(), 1.0);
    CHECK(output[0] == std::complex<double>(10.0, 0.0));
    CHECK(output[1] == std::complex<double>(-2.0, 2.0));
    CHECK(output[2] == std::complex<double>(-2.0, 0.0));
}

TEST_CASE("minizip-ng writes a zip and reads it back") {
    const TempFolder folder;
    // minizip-ng takes UTF-8 paths; path::string() would be the ANSI code page on Windows.
    const std::u8string zip_u8 = (folder.path / "test.zip").u8string();
    const std::string zip_path(zip_u8.begin(), zip_u8.end());
    const std::string content = "Ciao, FioriVoce. Ciao, FioriVoce. Ciao, FioriVoce.";

    void* writer = mz_zip_writer_create();
    REQUIRE(writer != nullptr);
    mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
    REQUIRE(mz_zip_writer_open_file(writer, zip_path.c_str(), 0, 0) == MZ_OK);
    mz_zip_file info{};
    info.filename = "data/hello.txt";
    info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    info.version_madeby = MZ_VERSION_MADEBY;
    CHECK(mz_zip_writer_add_buffer(writer, content.data(), static_cast<int32_t>(content.size()),
                                   &info) == MZ_OK);
    CHECK(mz_zip_writer_close(writer) == MZ_OK);
    mz_zip_writer_delete(&writer);

    void* reader = mz_zip_reader_create();
    REQUIRE(reader != nullptr);
    REQUIRE(mz_zip_reader_open_file(reader, zip_path.c_str()) == MZ_OK);
    REQUIRE(mz_zip_reader_locate_entry(reader, "data/hello.txt", 0) == MZ_OK);
    const int32_t length = mz_zip_reader_entry_save_buffer_length(reader);
    REQUIRE(length == static_cast<int32_t>(content.size()));
    std::string read_back(static_cast<std::size_t>(length), '\0');
    CHECK(mz_zip_reader_entry_save_buffer(reader, read_back.data(), length) == MZ_OK);
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    CHECK(read_back == content);
}

TEST_CASE("zlib compresses and restores a buffer") {
    const std::string text(1000, 'a');
    uLongf compressed_size = compressBound(static_cast<uLong>(text.size()));
    std::vector<Bytef> compressed(compressed_size);
    REQUIRE(compress2(compressed.data(), &compressed_size,
                      reinterpret_cast<const Bytef*>(text.data()), static_cast<uLong>(text.size()),
                      Z_BEST_COMPRESSION) == Z_OK);
    CHECK(compressed_size < text.size());
    std::string restored(text.size(), '\0');
    uLongf restored_size = static_cast<uLongf>(restored.size());
    REQUIRE(uncompress(reinterpret_cast<Bytef*>(restored.data()), &restored_size, compressed.data(),
                       compressed_size) == Z_OK);
    CHECK(restored == text);
    CHECK_THAT(std::string(zlibVersion()), StartsWith("1."));
}

TEST_CASE("pugixml parses an OOXML fragment") {
    pugi::xml_document document;
    const auto result = document.load_string(
        R"(<w:document xmlns:w="urn:w"><w:body><w:p><w:r><w:t>Ciao</w:t></w:r></w:p></w:body></w:document>)");
    REQUIRE(result);
    CHECK(std::string(document.select_node("//w:t").node().child_value()) == "Ciao");
}

#ifdef _WIN32
TEST_CASE("wil wraps a Windows event") {
    wil::unique_event event;
    event.create(wil::EventOptions::ManualReset);
    CHECK_FALSE(event.is_signaled());
    event.SetEvent();
    CHECK(event.is_signaled());
}
#endif
