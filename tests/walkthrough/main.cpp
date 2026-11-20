// Catch2's runner, and the child process the hard-exit recovery test starts:
// fv_walkthrough_tests --hard-exit-child <journal output file> <walkthrough folder>.
#include <catch2/catch_session.hpp>

#include <filesystem>
#include <string_view>

namespace fv::walkthrough_test {

std::filesystem::path g_self;
int hard_exit_child(const std::filesystem::path& output, const std::filesystem::path& folder);

} // namespace fv::walkthrough_test

int main(int argc, char* argv[]) {
    fv::walkthrough_test::g_self = std::filesystem::absolute(argv[0]);
    if (argc == 4 && std::string_view(argv[1]) == "--hard-exit-child") {
        return fv::walkthrough_test::hard_exit_child(argv[2], argv[3]);
    }
    return Catch::Session().run(argc, argv);
}
