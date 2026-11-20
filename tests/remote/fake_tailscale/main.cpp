// A stand-in for tailscale.exe in test_tailscale_cli.cpp's real-launcher test. It writes its
// arguments, one per line, to args.txt beside itself and answers as mode.txt beside it says:
//   status  `tailscale status --json` of a running tailnet owned by owner@example.com
//   whois   `tailscale whois --json` of that owner's computer "devbox"
//   exit1   "failed to connect to local tailscaled" on stderr, exit code 1
// Anything else exits with 2.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::filesystem::path own_folder(const char* argv0) {
#ifdef _WIN32
    wchar_t buffer[32768];
    const DWORD size = GetModuleFileNameW(nullptr, buffer, 32768);
    if (size > 0 && size < 32768) {
        return std::filesystem::path(std::wstring(buffer, size)).parent_path();
    }
#endif
    return std::filesystem::absolute(argv0).parent_path();
}

} // namespace

int main(int argc, char** argv) {
    const auto folder = own_folder(argv[0]);
    {
        std::ofstream args(folder / "args.txt", std::ios::binary | std::ios::trunc);
        for (int i = 1; i < argc; ++i) {
            args << argv[i] << '\n';
        }
    }
    std::ifstream mode_file(folder / "mode.txt");
    std::string mode((std::istreambuf_iterator<char>(mode_file)), {});
    while (!mode.empty() && (mode.back() == '\n' || mode.back() == '\r' || mode.back() == ' ')) {
        mode.pop_back();
    }
    if (mode == "status") {
        std::fputs(R"({"BackendState": "Running",
 "Self": {"HostName": "LAPTOP-1", "DNSName": "laptop.tail0000.ts.net.", "UserID": 1,
          "TailscaleIPs": ["100.64.0.1"]},
 "User": {"1": {"LoginName": "owner@example.com"}},
 "Peer": {"a": {"HostName": "devbox", "DNSName": "devbox.tail0000.ts.net.", "OS": "linux",
                "Online": true, "UserID": 1, "TailscaleIPs": ["100.64.0.2"]}}}
)",
                   stdout);
        return 0;
    }
    if (mode == "whois") {
        std::fputs(R"({"Node": {"Name": "devbox.tail0000.ts.net.", "ComputedName": "devbox"},
 "UserProfile": {"LoginName": "owner@example.com"}}
)",
                   stdout);
        return 0;
    }
    if (mode == "exit1") {
        std::fputs("failed to connect to local tailscaled\n", stderr);
        return 1;
    }
    return 2;
}
