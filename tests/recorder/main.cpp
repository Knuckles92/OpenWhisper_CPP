// Catch2's runner, and the child processes the kill-recovery tests start and kill:
// fv_recorder_tests --journal-child <output file> <ready file> (a bare journal), and
// fv_recorder_tests --recorder-child <data root> <ready file> (a recorder mid-capture).
#include <catch2/catch_session.hpp>

#include <filesystem>
#include <string_view>

namespace fv::recorder_test {

std::filesystem::path g_self;
int journal_child(const std::filesystem::path& output, const std::filesystem::path& ready);
int recorder_child(const std::filesystem::path& root, const std::filesystem::path& ready);

} // namespace fv::recorder_test

int main(int argc, char* argv[]) {
    fv::recorder_test::g_self = std::filesystem::absolute(argv[0]);
    if (argc == 4 && std::string_view(argv[1]) == "--journal-child") {
        return fv::recorder_test::journal_child(argv[2], argv[3]);
    }
    if (argc == 4 && std::string_view(argv[1]) == "--recorder-child") {
        return fv::recorder_test::recorder_child(argv[2], argv[3]);
    }
    return Catch::Session().run(argc, argv);
}
