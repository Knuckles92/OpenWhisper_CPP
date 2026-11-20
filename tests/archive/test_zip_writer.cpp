// ZipWriter: zips as Python's zipfile writes them for a backup (stored and deflated entries,
// external attributes, Zip64 extra fields on every entry), read back through ZipReader.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

#include "archive/errors.h"
#include "archive/zip_reader.h"
#include "archive/zip_writer.h"
#include "testkit/testkit.h"

using fv::archive::EntryInfo;
using fv::archive::Method;
using fv::archive::ZipReader;
using fv::archive::ZipWriter;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kRegularFileAttr = (0100000u | 0600u) << 16;

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string read_entry(ZipReader& reader, std::string_view name) {
    std::string out;
    reader.read_entry(name, [&](std::span<const std::byte> block) {
        out.append(reinterpret_cast<const char*>(block.data()), block.size());
    });
    return out;
}

std::uint32_t crc_of(const std::string& bytes) {
    return static_cast<std::uint32_t>(
        crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uInt>(bytes.size())));
}

std::uint32_t u32(const std::string& bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

std::uint16_t u16(const std::string& bytes, std::size_t at) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + at, 2);
    return value;
}

std::uint64_t u64(const std::string& bytes, std::size_t at) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + at, 8);
    return value;
}

// The extra-field block holds a record with this header id.
bool has_extra(const std::string& extra, std::uint16_t id) {
    std::size_t at = 0;
    while (at + 4 <= extra.size()) {
        if (u16(extra, at) == id) {
            return true;
        }
        at += 4 + u16(extra, at + 2);
    }
    return false;
}

struct RawEntry {
    std::string name;
    bool central_zip64 = false;
    bool local_zip64 = false;
};

// Walks the central directory (through the Zip64 end records when there are any) and each
// entry's local header, reporting which carry a Zip64 extra field (header id 1).
std::vector<RawEntry> raw_entries(const std::string& zip) {
    const std::size_t eocd = zip.rfind(std::string("PK\x05\x06", 4));
    REQUIRE(eocd != std::string::npos);
    std::uint64_t cd_offset = u32(zip, eocd + 16);
    std::uint64_t count = u16(zip, eocd + 10);
    if (cd_offset == 0xFFFFFFFF || count == 0xFFFF) {
        REQUIRE(eocd >= 20);
        REQUIRE(u32(zip, eocd - 20) == 0x07064b50);
        const std::uint64_t zip64_eocd = u64(zip, eocd - 20 + 8);
        REQUIRE(u32(zip, zip64_eocd) == 0x06064b50);
        count = u64(zip, zip64_eocd + 32);
        cd_offset = u64(zip, zip64_eocd + 48);
    }
    std::vector<RawEntry> entries;
    std::size_t at = static_cast<std::size_t>(cd_offset);
    for (std::uint64_t i = 0; i < count; ++i) {
        REQUIRE(u32(zip, at) == 0x02014b50);
        const std::uint16_t name_length = u16(zip, at + 28);
        const std::uint16_t extra_length = u16(zip, at + 30);
        const std::uint16_t comment_length = u16(zip, at + 32);
        RawEntry entry;
        entry.name = zip.substr(at + 46, name_length);
        const std::string extra = zip.substr(at + 46 + name_length, extra_length);
        entry.central_zip64 = has_extra(extra, 1);
        std::uint64_t local = u32(zip, at + 42);
        if (local == 0xFFFFFFFF) {
            // The Zip64 record holds the sizes that overflowed first, then the offset.
            std::size_t field = 0;
            while (u16(extra, field) != 1) {
                field += 4 + u16(extra, field + 2);
            }
            std::size_t value = field + 4;
            if (u32(zip, at + 24) == 0xFFFFFFFF) {
                value += 8;
            }
            if (u32(zip, at + 20) == 0xFFFFFFFF) {
                value += 8;
            }
            local = u64(extra, value);
        }
        REQUIRE(u32(zip, static_cast<std::size_t>(local)) == 0x04034b50);
        const std::uint16_t local_name = u16(zip, static_cast<std::size_t>(local) + 26);
        const std::uint16_t local_extra = u16(zip, static_cast<std::size_t>(local) + 28);
        entry.local_zip64 = has_extra(
            zip.substr(static_cast<std::size_t>(local) + 30 + local_name, local_extra), 1);
        entries.push_back(std::move(entry));
        at += 46 + name_length + extra_length + comment_length;
    }
    return entries;
}

} // namespace

TEST_CASE("stored and deflated entries round trip through the reader", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path source = root.path() / "source.txt";
    const std::string text(300'000, 'x');
    write_file(source, text + "tail");
    const std::string audio = "RIFF" + std::string(5000, '\x01');
    const fs::path zip_path = root.path() / "out.zip";
    std::vector<std::size_t> chunks;
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_file("data/source.txt", source, Method::Deflated, kRegularFileAttr,
                        [&](std::span<const std::byte> block) { chunks.push_back(block.size()); });
        writer.add_bytes("data/recordings/a.wav", bytes_of(audio), Method::Stored,
                         kRegularFileAttr);
        writer.add_bytes("data/empty", {}, Method::Deflated, kRegularFileAttr);
        writer.add_bytes("data/caf\xc3\xa9.txt", bytes_of("accent"), Method::Deflated,
                         kRegularFileAttr);
        writer.add_bytes("manifest.json", bytes_of("{}"), Method::Deflated, 0);
        writer.close();
        writer.close(); // idempotent
    }
    CHECK(chunks == std::vector<std::size_t>{text.size() + 4});

    ZipReader reader(zip_path);
    const auto& infos = reader.infolist();
    REQUIRE(infos.size() == 5);
    CHECK(infos[0].name == "data/source.txt");
    CHECK(infos[0].method == Method::Deflated);
    CHECK(infos[0].size == text.size() + 4);
    CHECK(infos[0].compressed_size < infos[0].size);
    CHECK(infos[0].crc32 == crc_of(text + "tail"));
    CHECK(infos[0].external_attr == kRegularFileAttr);
    CHECK_FALSE(infos[0].directory);
    CHECK(infos[1].method == Method::Stored);
    CHECK(infos[1].size == audio.size());
    CHECK(infos[1].compressed_size == audio.size());
    CHECK(infos[2].size == 0);
    CHECK(infos[3].name == "data/caf\xc3\xa9.txt");
    CHECK(infos[4].external_attr == 0);
    CHECK(read_entry(reader, "data/source.txt") == text + "tail");
    CHECK(read_entry(reader, "data/recordings/a.wav") == audio);
    CHECK(read_entry(reader, "data/empty").empty());
    CHECK(read_entry(reader, "data/caf\xc3\xa9.txt") == "accent");
    CHECK(read_entry(reader, "manifest.json") == "{}");
    CHECK_THROWS_AS(read_entry(reader, "missing"), std::out_of_range);
}

TEST_CASE("every entry carries Zip64 extra fields", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path zip_path = root.path() / "out.zip";
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_bytes("a", bytes_of("small"), Method::Deflated, kRegularFileAttr);
        writer.add_bytes("b", bytes_of("small"), Method::Stored, kRegularFileAttr);
        writer.close();
    }
    const auto entries = raw_entries(read_file(zip_path));
    REQUIRE(entries.size() == 2);
    for (const auto& entry : entries) {
        INFO(entry.name);
        CHECK(entry.local_zip64);
        CHECK(entry.central_zip64);
    }
}

TEST_CASE("without zip64 no entry carries zip64 extra fields", "[zip_writer]") {
    // As zipfile writes entries without force_zip64, the way python-docx writes a .docx.
    fv::testkit::TempDataRoot root;
    const fs::path zip_path = root.path() / "plain.zip";
    const std::string text(70'000, 'w');
    {
        ZipWriter writer = ZipWriter::create(zip_path, {.zip64 = false});
        writer.add_bytes("[Content_Types].xml", bytes_of("<Types/>"), Method::Deflated,
                         kRegularFileAttr);
        writer.add_bytes("word/document.xml", bytes_of(text), Method::Deflated, kRegularFileAttr);
        writer.add_bytes("word/media/image1.png", bytes_of("png"), Method::Stored, 0);
        writer.close();
    }
    const std::string zip = read_file(zip_path);
    const auto entries = raw_entries(zip);
    REQUIRE(entries.size() == 3);
    for (const auto& entry : entries) {
        INFO(entry.name);
        CHECK_FALSE(entry.local_zip64);
        CHECK_FALSE(entry.central_zip64);
    }
    // No Zip64 end of central directory, and version 2.0 made and needed.
    const std::size_t eocd = zip.rfind(std::string("PK\x05\x06", 4));
    CHECK((eocd < 20 || u32(zip, eocd - 20) != 0x07064b50));
    const std::size_t central = u32(zip, eocd + 16);
    CHECK((u16(zip, central + 4) & 0xFF) == 20);
    CHECK(u16(zip, central + 6) == 20);

    ZipReader reader(zip_path);
    REQUIRE(reader.infolist().size() == 3);
    CHECK(reader.infolist()[1].size == text.size());
    CHECK(reader.infolist()[1].compressed_size < text.size());
    CHECK(read_entry(reader, "word/document.xml") == text);
    CHECK(read_entry(reader, "word/media/image1.png") == "png");
}

TEST_CASE("external attributes reach the reader as written", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path zip_path = root.path() / "attrs.zip";
    constexpr std::uint32_t kLink = (0120000u | 0777u) << 16;
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_bytes("regular", bytes_of("r"), Method::Deflated, kRegularFileAttr);
        writer.add_bytes("link", bytes_of("../x"), Method::Stored, kLink);
        writer.add_bytes("dir/", {}, Method::Stored, 0);
        writer.close();
    }
    ZipReader reader(zip_path);
    CHECK(reader.infolist()[0].external_attr == kRegularFileAttr);
    CHECK(reader.infolist()[0].external_attr >> 16 == 0100600u);
    CHECK(reader.infolist()[1].external_attr == kLink);
    CHECK(reader.infolist()[2].directory);
}

TEST_CASE("a writer that fails mid-member leaves no file behind", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path source = root.path() / "big.bin";
    write_file(source, std::string(3 << 20, 'z'));
    const fs::path zip_path = root.path() / "partial.zip";
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_bytes("first", bytes_of("done"), Method::Deflated, kRegularFileAttr);
        int calls = 0;
        CHECK_THROWS_AS(writer.add_file("big.bin", source, Method::Deflated, kRegularFileAttr,
                                        [&](std::span<const std::byte>) {
                                            if (++calls == 2) {
                                                throw std::runtime_error("cancel");
                                            }
                                        }),
                        std::runtime_error);
        CHECK(calls == 2);
        CHECK_THROWS_AS(writer.add_bytes("after", bytes_of("x"), Method::Stored, 0),
                        std::logic_error);
        CHECK_THROWS_AS(writer.close(), std::logic_error);
    }
    CHECK_FALSE(fs::exists(zip_path));
}

TEST_CASE("a writer dropped without closing removes its file", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path zip_path = root.path() / "dropped.zip";
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_bytes("a", bytes_of("a"), Method::Stored, 0);
    }
    CHECK_FALSE(fs::exists(zip_path));
}

TEST_CASE("a name held by two entries reads as the last of them", "[zip_writer]") {
    // zipfile's NameToInfo keeps the last ZipInfo of a name; infolist() keeps both.
    fv::testkit::TempDataRoot root;
    const fs::path zip_path = root.path() / "twice.zip";
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_bytes("data/dup", bytes_of("first"), Method::Deflated, kRegularFileAttr);
        writer.add_bytes("data/other", bytes_of("other"), Method::Stored, kRegularFileAttr);
        writer.add_bytes("data/dup", bytes_of("second"), Method::Stored, kRegularFileAttr);
        writer.close();
    }
    ZipReader reader(zip_path);
    REQUIRE(reader.infolist().size() == 3);
    CHECK(read_entry(reader, "data/dup") == "second");
    CHECK(read_entry(reader, "data/other") == "other");
    CHECK(read_entry(reader, "data/dup") == "second");
}

TEST_CASE("the reader refuses a local header that names another file", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path zip_path = root.path() / "renamed.zip";
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_bytes("data/a", bytes_of("payload"), Method::Stored, 0);
        writer.close();
    }
    std::string bytes = read_file(zip_path);
    // The local header comes first: its name starts 30 bytes in.
    REQUIRE(bytes.compare(30, 6, "data/a") == 0);
    bytes[35] = 'b';
    write_file(zip_path, bytes);
    ZipReader reader(zip_path);
    CHECK_THROWS_MATCHES(
        read_entry(reader, "data/a"), fv::archive::BadArchive,
        Catch::Matchers::Message("File name in directory 'data/a' and header b'data/b' differ."));
}

TEST_CASE("a member over 4 GiB is declared through Zip64", "[zip_writer][platform]") {
    fv::testkit::TempDataRoot root;
    const fs::path source = root.path() / "sparse.bin";
    constexpr std::uint64_t kSize = (std::uint64_t{1} << 32) + 4096;
    { std::ofstream(source, std::ios::binary).put('\0'); }
#ifdef _WIN32
    // Marked sparse first, so the 4 GiB of zeros take no disk space.
    HANDLE file = CreateFileW(source.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    const BOOL sparse =
        DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    CloseHandle(file);
    if (!sparse) {
        SKIP("the temporary folder's file system has no sparse files");
    }
#endif
    fs::resize_file(source, kSize);
    const fs::path zip_path = root.path() / "large.zip";
    std::uint64_t seen = 0;
    {
        ZipWriter writer = ZipWriter::create(zip_path);
        writer.add_file("data/recordings/large.wav", source, Method::Deflated, kRegularFileAttr,
                        [&](std::span<const std::byte> block) { seen += block.size(); });
        writer.add_bytes("after", bytes_of("tail"), Method::Stored, kRegularFileAttr);
        writer.close();
    }
    CHECK(seen == kSize);
    ZipReader reader(zip_path);
    REQUIRE(reader.infolist().size() == 2);
    const EntryInfo& large = reader.infolist()[0];
    CHECK(large.size == kSize);
    CHECK(large.compressed_size < kSize / 100);
    std::uint64_t read = 0;
    bool zeros = true;
    reader.read_entry("data/recordings/large.wav", [&](std::span<const std::byte> block) {
        read += block.size();
        for (const std::byte b : block) {
            zeros = zeros && b == std::byte{0};
        }
    });
    CHECK(read == kSize);
    CHECK(zeros);
    CHECK(read_entry(reader, "after") == "tail");
}
