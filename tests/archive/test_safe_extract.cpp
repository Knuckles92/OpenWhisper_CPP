// Safe extraction of component zips and native speech runtime tarballs: OpenWhisper's archive
// safety tests from tests/test_components.py, and the readers underneath.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "archive/errors.h"
#include "archive/safe_extract.h"
#include "archive/tar_gz_reader.h"
#include "archive/zip_reader.h"
#include "builders.h"
#include "testkit/testkit.h"

using fv::archive::ArchiveError;
using fv::archive::BadArchive;
using fv::archive::ExtractCanceled;
using fv::testing::TarBuilder;
using fv::testing::ZipMember;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void no_progress(std::int64_t, std::int64_t) {}

fs::path zip_with_member(const fs::path& path, const std::string& member_name) {
    fv::testing::make_zip(path, {ZipMember{member_name, "payload"}});
    return path;
}

void extract_tar(const fs::path& archive, const fs::path& destination,
                 std::string_view root = "nemo-speech", std::string_view folder = "nemo-speech") {
    fv::archive::extract_native_tar(archive, destination, root, folder, no_progress, {});
}

// A NeMo-Speech.cpp Linux release in miniature: versioned top folder, soname links.
void linux_release(const fs::path& archive, const std::string& root,
                   const std::vector<TarBuilder::Entry>& extra = {}) {
    TarBuilder tar;
    tar.add(root + "/lib/libnemo_speech_asr_c.so.1", "real");
    tar.add({root + "/lib/libnemo_speech_asr_c.so", TarBuilder::kSymlink, "",
             "libnemo_speech_asr_c.so.1"});
    tar.add(root + "/lib/libggml.so.0.12.0", "ggml");
    tar.add({root + "/lib/libggml.so.0", TarBuilder::kSymlink, "", "libggml.so.0.12.0"});
    tar.add(root + "/share/licenses/LICENSE", "Apache-2.0");
    for (const auto& entry : extra) {
        tar.add(entry);
    }
    tar.save_gz(archive);
}

} // namespace

// from tests/test_components.py::test_safe_extract_rejects_traversal_and_absolute_paths
TEST_CASE("test_safe_extract_rejects_traversal_and_absolute_paths", "[test_components]") {
    const auto member = GENERATE(as<std::string>{}, "../escaped.txt", "/etc/passwd");
    CAPTURE(member);
    const fv::testkit::TempDataRoot root;
    const auto archive = zip_with_member(root.path() / "evil.zip", member);
    try {
        fv::archive::extract_zip(archive, root.path() / "out", no_progress, {});
        FAIL("the archive was extracted");
    } catch (const ArchiveError& error) {
        CHECK(std::string(error.what()) == "Archive contains an unsafe path: " + member);
    }
    CHECK_FALSE(fs::exists(root.path() / "escaped.txt"));
}

// from tests/test_components.py::test_safe_extract_writes_normal_members
TEST_CASE("test_safe_extract_writes_normal_members", "[test_components]") {
    const fv::testkit::TempDataRoot root;
    const auto archive = zip_with_member(root.path() / "ok.zip", "bin/lib.dll");
    const auto out = root.path() / "out";
    fv::archive::extract_zip(archive, out, no_progress, {});
    CHECK(read_file(out / "bin" / "lib.dll") == "payload");
}

// from tests/test_components.py::test_safe_extract_honors_cancellation
TEST_CASE("test_safe_extract_honors_cancellation", "[test_components]") {
    const fv::testkit::TempDataRoot root;
    const auto archive = zip_with_member(root.path() / "ok.zip", "bin/lib.dll");
    fv::CancelSource cancel;
    cancel.cancel();
    CHECK_THROWS_AS(
        fv::archive::extract_zip(archive, root.path() / "out", no_progress, cancel.token()),
        ExtractCanceled);
    CHECK_FALSE(fs::exists(root.path() / "out" / "bin" / "lib.dll"));
}

// from tests/test_components.py::test_mac_archive_preserves_only_safe_library_links
TEST_CASE("test_mac_archive_preserves_only_safe_library_links", "[test_components]") {
    const auto target =
        GENERATE(as<std::string>{}, "libreal.dylib", "/tmp/escape", "../../../escape");
    CAPTURE(target);
    const fv::testkit::TempDataRoot root;
    const auto archive_path = root.path() / "native.tar.gz";
    TarBuilder tar;
    tar.add("nemo-speech/lib/libreal.dylib", "test");
    tar.add({"nemo-speech/lib/libalias.dylib", TarBuilder::kSymlink, "", target});
    tar.save_gz(archive_path);
    const auto destination = root.path() / "out";
    fs::create_directories(destination);
    if (target == "libreal.dylib") {
        extract_tar(archive_path, destination);
        CHECK(read_file(destination / "nemo-speech/lib/libalias.dylib") == "test");
    } else {
        CHECK_THROWS_AS(extract_tar(archive_path, destination), ArchiveError);
    }
}

// from tests/test_components.py::test_linux_release_folder_is_renamed_to_nemo_speech
TEST_CASE("test_linux_release_folder_is_renamed_to_nemo_speech", "[test_components]") {
    // The Python test then runs the payload validator over the result; that check is the
    // component installer's, not the extractor's.
    const std::string root_name = "nemo-speech-0.1.0-linux-x86_64-cpu";
    const fv::testkit::TempDataRoot root;
    const auto archive_path = root.path() / "native.tar.gz";
    linux_release(archive_path, root_name,
                  {{root_name + "/lib/libhard.so", TarBuilder::kHardLink, "",
                    root_name + "/lib/libggml.so.0.12.0"}});
    const auto destination = root.path() / "out";
    fs::create_directories(destination);
    extract_tar(archive_path, destination, root_name);
    const auto lib = destination / "nemo-speech" / "lib";
    CHECK(read_file(lib / "libnemo_speech_asr_c.so") == "real");
    CHECK(read_file(lib / "libggml.so.0") == "ggml");
    CHECK(read_file(lib / "libhard.so") == "ggml");
    CHECK(read_file(destination / "nemo-speech" / "share" / "licenses" / "LICENSE") ==
          "Apache-2.0");
    CHECK_FALSE(fs::exists(destination / root_name));
}

// from tests/test_components.py::test_linux_release_rejects_anything_outside_its_folder
TEST_CASE("test_linux_release_rejects_anything_outside_its_folder", "[test_components]") {
    struct Case {
        std::string name;
        char type;
        std::string linkname;
        std::string message_start;
    };
    const auto c = GENERATE(
        // another release's folder
        Case{"nemo-speech-0.1.0-linux-x86_64-cuda/lib/x.so", TarBuilder::kRegular, "",
             "Archive contains an unsafe path: nemo-speech-0.1.0-linux-x86_64-cuda/lib/x.so"},
        Case{"nemo-speech/lib/x.so", TarBuilder::kRegular, "",
             "Archive contains an unsafe path: nemo-speech/lib/x.so"},
        Case{"nemo-speech-0.1.0-linux-x86_64-cpu/../escape", TarBuilder::kRegular, "",
             "Archive contains an unsafe path: nemo-speech-0.1.0-linux-x86_64-cpu/../escape"},
        Case{"nemo-speech-0.1.0-linux-x86_64-cpu/lib/hard", TarBuilder::kHardLink, "/etc/passwd",
             "Archive contains an unsafe link: /etc/passwd"},
        Case{"nemo-speech-0.1.0-linux-x86_64-cpu/lib/soft", TarBuilder::kSymlink, "../../../escape",
             "Unsafe speech runtime archive: 'nemo-speech/lib/soft' would link to '"});
    CAPTURE(c.name);
    const std::string root_name = "nemo-speech-0.1.0-linux-x86_64-cpu";
    const fv::testkit::TempDataRoot root;
    const auto archive_path = root.path() / "native.tar.gz";
    linux_release(archive_path, root_name, {{c.name, c.type, "", c.linkname}});
    const auto destination = root.path() / "out";
    fs::create_directories(destination);
    try {
        extract_tar(archive_path, destination, root_name);
        FAIL("the archive was extracted");
    } catch (const ArchiveError& error) {
        CHECK(std::string(error.what()).starts_with(c.message_start));
    }
    CHECK_FALSE(fs::exists(root.path() / "escape"));
}

TEST_CASE("a link outside the destination names where it would point", "[safe_extract][platform]") {
    const fv::testkit::TempDataRoot root;
    const auto archive_path = root.path() / "native.tar.gz";
    TarBuilder tar;
    tar.add({"nemo-speech/lib/soft", TarBuilder::kSymlink, "", "../../../escape"});
    tar.save_gz(archive_path);
    const auto destination = root.path() / "out";
    fs::create_directories(destination);
    const auto escape = fs::weakly_canonical(destination / "nemo-speech/lib/../../../escape");
    try {
        extract_tar(archive_path, destination);
        FAIL("the archive was extracted");
    } catch (const ArchiveError& error) {
        std::string expected_path;
        for (const char ch : fv::path_to_utf8(escape)) {
            if (ch == '\\') {
                expected_path += '\\';
            }
            expected_path += ch;
        }
        CHECK(std::string(error.what()) ==
              "Unsafe speech runtime archive: 'nemo-speech/lib/soft' would link to '" +
                  expected_path + "', which is outside the destination");
    }
}

TEST_CASE("the data filter refuses special files and links that leave the folder",
          "[safe_extract]") {
    const fv::testkit::TempDataRoot root;
    const auto destination = root.path() / "out";
    fs::create_directories(destination);

    TarBuilder fifo;
    fifo.add({"nemo-speech/pipe", TarBuilder::kFifo});
    fifo.save_gz(root.path() / "fifo.tar.gz");
    try {
        extract_tar(root.path() / "fifo.tar.gz", destination);
        FAIL("the archive was extracted");
    } catch (const ArchiveError& error) {
        CHECK(std::string(error.what()) ==
              "Unsafe speech runtime archive: 'nemo-speech/pipe' is a special file");
    }

    // With root and folder equal, a hard link is checked by the filter alone.
    TarBuilder hard;
    hard.add("nemo-speech/a", "a");
    hard.add({"nemo-speech/h", TarBuilder::kHardLink, "", "../outside"});
    hard.save_gz(root.path() / "hard.tar.gz");
    try {
        extract_tar(root.path() / "hard.tar.gz", destination);
        FAIL("the archive was extracted");
    } catch (const ArchiveError& error) {
        const std::string message = error.what();
        CHECK(
            message.starts_with("Unsafe speech runtime archive: 'nemo-speech/h' would link to '"));
        CHECK(message.ends_with("outside', which is outside the destination"));
    }
    CHECK(read_file(destination / "nemo-speech" / "a") == "a");

    TarBuilder absolute;
    absolute.add({"nemo-speech/s", TarBuilder::kSymlink, "", "/etc/passwd"});
    absolute.save_gz(root.path() / "absolute.tar.gz");
    try {
        extract_tar(root.path() / "absolute.tar.gz", destination);
        FAIL("the archive was extracted");
    } catch (const ArchiveError& error) {
        CHECK(std::string(error.what()) ==
              "Unsafe speech runtime archive: 'nemo-speech/s' is a link to an absolute path");
    }
}

TEST_CASE("a link that cannot be made is written as a copy of its target", "[safe_extract]") {
    // os.link fails because the name is taken, so tarfile copies the earlier member's data out
    // of the archive again (CPython 3.12.10 leaves b"A" in a file with one link).
    const fv::testkit::TempDataRoot root;
    TarBuilder tar;
    tar.add("nemo-speech/a", "A");
    tar.add("nemo-speech/h", "old");
    tar.add({"nemo-speech/h", TarBuilder::kHardLink, "", "nemo-speech/a"});
    tar.save_gz(root.path() / "links.tar.gz");
    extract_tar(root.path() / "links.tar.gz", root.path() / "out");
    CHECK(read_file(root.path() / "out" / "nemo-speech" / "h") == "A");
    CHECK(fs::hard_link_count(root.path() / "out" / "nemo-speech" / "h") == 1);
}

TEST_CASE("tar extraction reports progress and honours cancellation", "[safe_extract]") {
    const fv::testkit::TempDataRoot root;
    TarBuilder tar;
    tar.add({"nemo-speech/lib", TarBuilder::kDirectory});
    tar.add("nemo-speech/lib/a", "a");
    tar.add("nemo-speech/lib/b", "bb", 0755);
    tar.save_gz(root.path() / "r.tar.gz");
    std::vector<std::pair<std::int64_t, std::int64_t>> seen;
    fv::archive::extract_native_tar(
        root.path() / "r.tar.gz", root.path() / "out", "nemo-speech", "nemo-speech",
        [&](std::int64_t done, std::int64_t total) { seen.emplace_back(done, total); }, {});
    CHECK(seen == std::vector<std::pair<std::int64_t, std::int64_t>>{{1, 3}, {2, 3}, {3, 3}});
    CHECK(read_file(root.path() / "out" / "nemo-speech" / "lib" / "b") == "bb");
    const auto written = fs::last_write_time(root.path() / "out" / "nemo-speech" / "lib" / "a");
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch());
    CHECK(seconds.count() == 1'700'000'000);

    fv::CancelSource cancel;
    cancel.cancel();
    CHECK_THROWS_AS(fv::archive::extract_native_tar(root.path() / "r.tar.gz", root.path() / "again",
                                                    "nemo-speech", "nemo-speech", no_progress,
                                                    cancel.token()),
                    ExtractCanceled);
    CHECK_FALSE(fs::exists(root.path() / "again" / "nemo-speech"));
}

TEST_CASE("long names and pax headers are read as tarfile reads them", "[tar_gz_reader]") {
    const fv::testkit::TempDataRoot root;
    const std::string long_name = "nemo-speech/" + std::string(120, 'n') + "/file.so";
    TarBuilder tar;
    tar.add_long_name({long_name, TarBuilder::kRegular, "long"});
    TarBuilder::Entry short_entry{"short-name", TarBuilder::kRegular, "pax"};
    tar.add_pax('x', {{"path", "nemo-speech/" + std::string(150, 'p')}, {"mtime", "12.5"}},
                &short_entry);
    tar.add_pax('g', {{"linkpath", "global-target"}});
    tar.add({"nemo-speech/link", TarBuilder::kSymlink, "", "ignored"});
    tar.add({"nemo-speech/dir/", TarBuilder::kDirectory});
    tar.save_gz(root.path() / "names.tar.gz");

    const auto members = fv::archive::list_tar_gz(root.path() / "names.tar.gz");
    REQUIRE(members.size() == 4);
    CHECK(members[0].name == long_name);
    CHECK(members[0].size == 4);
    CHECK(members[1].name == "nemo-speech/" + std::string(150, 'p'));
    CHECK(members[1].mtime == 12.5);
    CHECK(members[2].linkname == "global-target");
    CHECK(members[3].name == "nemo-speech/dir");
    CHECK(members[3].is_directory());
    for (std::size_t i = 0; i < members.size(); ++i) {
        CHECK(members[i].index == i);
    }

    fv::archive::TarGzReader reader(root.path() / "names.tar.gz");
    REQUIRE(reader.next());
    std::string data(16, '\0');
    data.resize(reader.read(data));
    CHECK(data == "long");
}

TEST_CASE("damaged tarballs fail as tarfile fails", "[tar_gz_reader]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "bad.tar.gz";
    const auto failure = [&] {
        try {
            fv::archive::list_tar_gz(path);
        } catch (const BadArchive& error) {
            return std::string(error.what());
        }
        return std::string("no error");
    };

    TarBuilder::write(path, "plain text, not gzip at all");
    CHECK(failure() == "not a gzip file");

    TarBuilder::write(path, TarBuilder::gzip(""));
    CHECK(failure() == "empty file");

    // Texts as CPython 3.12.10's tarfile gives them for the same bytes.
    TarBuilder::write(path, TarBuilder::gzip(std::string(512, 'z')));
    CHECK(failure() == "invalid header");

    TarBuilder wrong_sum;
    wrong_sum.add("nemo-speech/a", "a");
    auto block = wrong_sum.tar().substr(0, 512);
    block.replace(148, 8, std::string("0000000\0", 8));
    TarBuilder::write(path, TarBuilder::gzip(block + std::string(1024, '\0')));
    CHECK(failure() == "bad checksum");

    TarBuilder truncated;
    truncated.add("nemo-speech/a", std::string(2000, 'a'));
    TarBuilder::write(path, TarBuilder::gzip(truncated.tar().substr(0, 1024)));
    CHECK(failure() == "unexpected end of data");

    // A damaged header after the first ends the archive quietly.
    TarBuilder tail;
    tail.add("nemo-speech/a", "a");
    tail.append_raw(std::string(512, 'z'));
    tail.save_gz(path);
    CHECK(fv::archive::list_tar_gz(path).size() == 1);

    CHECK_THROWS_AS(fv::archive::list_tar_gz(root.path() / "missing.tar.gz"), std::system_error);
}

TEST_CASE("zip members are written as zipfile writes them", "[safe_extract]") {
    const fv::testkit::TempDataRoot root;
    std::vector<ZipMember> members = {
        {"./a/./b.txt", "b"},
        {"dir/", ""},
        {"\x82t\x82.txt", "cp437", false},
    };
#ifdef _WIN32
    members.push_back({"x/na:me?.txt. ", "sanitized"});
    members.push_back({"C:/drive.txt", "drive"});
#endif
    fv::testing::make_zip(root.path() / "names.zip", members);
    const auto out = root.path() / "out";
    std::vector<std::pair<std::int64_t, std::int64_t>> seen;
    fv::archive::extract_zip(
        root.path() / "names.zip", out,
        [&](std::int64_t done, std::int64_t total) { seen.emplace_back(done, total); }, {});
    CHECK(seen.size() == members.size());
    CHECK(seen.back() ==
          std::pair<std::int64_t, std::int64_t>(static_cast<std::int64_t>(members.size()),
                                                static_cast<std::int64_t>(members.size())));
    CHECK(read_file(out / "a" / "b.txt") == "b");
    CHECK(fs::is_directory(out / "dir"));
    CHECK(read_file(out / fv::path_from_utf8("\xC3\xA9t\xC3\xA9.txt")) == "cp437");
#ifdef _WIN32
    CHECK(read_file(out / "x" / "na_me_.txt") == "sanitized");
    CHECK(read_file(out / "drive.txt") == "drive");
#endif

    fv::archive::ZipReader reader(root.path() / "names.zip");
    CHECK(reader.entries()[2].name == "\xC3\xA9t\xC3\xA9.txt");
    CHECK(reader.entries()[1].is_dir);
}

TEST_CASE("damaged zips fail as zipfile fails", "[zip_reader]") {
    const fv::testkit::TempDataRoot root;
    TarBuilder::write(root.path() / "plain.zip", "not a zip");
    try {
        fv::archive::ZipReader reader(root.path() / "plain.zip");
        FAIL("opened");
    } catch (const BadArchive& error) {
        CHECK(std::string(error.what()) == "File is not a zip file");
    }

    const auto stored = root.path() / "stored.zip";
    fv::testing::make_zip(stored, {ZipMember{"bin/lib.dll", "payload-payload", true, true}});
    auto bytes = read_file(stored);
    const auto at = bytes.find("payload-payload");
    REQUIRE(at != std::string::npos);
    bytes[at] = 'P';
    TarBuilder::write(stored, bytes);
    try {
        fv::archive::extract_zip(stored, root.path() / "out", no_progress, {});
        FAIL("extracted");
    } catch (const BadArchive& error) {
        CHECK(std::string(error.what()) == "Bad CRC-32 for file 'bin/lib.dll'");
    }

    CHECK_THROWS_AS(fv::archive::ZipReader(root.path() / "missing.zip"), std::system_error);
}
