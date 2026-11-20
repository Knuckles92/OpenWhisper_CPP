// Windows Credential Manager laid out as Python keyring's WinVaultKeyring lays it out. The
// [platform] cases write to the real Credential Manager, only under a service name of their own
// ("OpenWhisper_CPPTest.<process id>"), and delete every target they may have created.

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <windows.h>

#include <wincred.h>

#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "credentials/detail/errors.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "credentials/win/wincred_backend.h"
#include "test_support.h"
#include "testkit/testkit.h"

using fv::WinCredBackend;
using fv::credentials_test::TempDir;
using fv::credentials_test::write_bytes;
using fv::wincred_detail::decode_blob;

namespace {

std::wstring wide(std::string_view text) {
    std::wstring out;
    for (const char c : text) { // the test's own names are ASCII
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string test_service() {
    return std::format("OpenWhisper_CPPTest.{}", GetCurrentProcessId());
}

struct RawCredential {
    std::optional<std::wstring> user;
    std::string blob;
    DWORD persist = 0;
    std::wstring comment;
};

std::optional<RawCredential> read_raw(const std::string& target) {
    CREDENTIALW* raw = nullptr;
    if (!CredReadW(wide(target).c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        return std::nullopt;
    }
    RawCredential out;
    if (raw->UserName != nullptr) {
        out.user = raw->UserName;
    }
    if (raw->CredentialBlobSize > 0) {
        out.blob.assign(reinterpret_cast<const char*>(raw->CredentialBlob),
                        raw->CredentialBlobSize);
    }
    out.persist = raw->Persist;
    if (raw->Comment != nullptr) {
        out.comment = raw->Comment;
    }
    CredFree(raw);
    return out;
}

void write_raw(const std::string& target, const wchar_t* user, std::string blob) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    std::wstring wide_target = wide(target);
    credential.TargetName = wide_target.data();
    credential.UserName = const_cast<wchar_t*>(user);
    credential.Persist = CRED_PERSIST_ENTERPRISE;
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = reinterpret_cast<BYTE*>(blob.data());
    REQUIRE(CredWriteW(&credential, 0));
}

std::string utf16le(std::wstring_view text) {
    return std::string(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
}

// Deletes every target a test may have made under its service, also when it fails.
class TestService {
public:
    TestService() : service_(test_service()) { clean(); }
    ~TestService() { clean(); }
    const std::string& name() const { return service_; }
    std::string compound(std::string_view user) const { return std::string(user) + "@" + service_; }

private:
    void clean() const {
        for (const std::string& target :
             {service_, compound("K"), compound("K2"), compound("None"), compound("PY")}) {
            // One target can hold a credential per persistence; each delete removes one.
            for (int i = 0; i < 4 && CredDeleteW(wide(target).c_str(), CRED_TYPE_GENERIC, 0); ++i) {
            }
        }
    }
    std::string service_;
};

} // namespace

TEST_CASE("decoding a stored blob follows keyring's DecodingCredential") {
    CHECK(decode_blob("") == "");
    CHECK(decode_blob(utf16le(L"sk-abc")) == "sk-abc");
    CHECK(decode_blob(std::string("\xFF\xFE", 2) + utf16le(L"bom")) == "bom");
    CHECK(decode_blob(std::string("\xFE\xFF\x00h\x00i", 6)) == "hi");
    CHECK(decode_blob(utf16le(L"é\U0001F600")) == "é\U0001F600");
    // Odd length or an unpaired surrogate is not UTF-16: read as UTF-8 instead.
    CHECK(decode_blob("abc") == "abc");
    // Even length, but the first unit (0xDC61) is an unpaired low surrogate.
    CHECK(decode_blob("a\xDC\x80"
                      "b") == "a\xDC\x80"
                              "b");
    // Neither UTF-16 (an unpaired surrogate) nor UTF-8 (0xD8 starts no sequence).
    CHECK_THROWS_AS(decode_blob(std::string("\x00\xD8\x41\x00", 4)),
                    fv::credentials_detail::BlobDecodeError);
    CHECK_THROWS_AS(decode_blob("\xFF\xFF\xFF"), fv::credentials_detail::BlobDecodeError);
}

TEST_CASE("the Windows backend is Credential Manager and always available") {
    WinCredBackend backend;
    const fv::CredentialStatus status = backend.probe();
    CHECK(status.available);
    CHECK(status.backend_name == "Windows Credential Manager");
    CHECK(fv::platform_backend_name() == "Windows Credential Manager");
    CHECK(dynamic_cast<WinCredBackend*>(fv::make_platform_backend().get()) != nullptr);
}

TEST_CASE("WinCredBackend writes, reads, overwrites and deletes as keyring does", "[platform]") {
    TestService service;
    WinCredBackend backend;
    const std::string& s = service.name();

    CHECK_FALSE(backend.get(s, "K").has_value());
    CHECK_FALSE(backend.remove(s, "K"));

    backend.set(s, "K", "v1");
    auto plain = read_raw(s);
    REQUIRE(plain.has_value());
    CHECK(plain->user == L"K");
    CHECK(plain->blob == utf16le(L"v1"));
    CHECK(plain->persist == CRED_PERSIST_ENTERPRISE);
    CHECK(plain->comment == L"Stored using python-keyring");
    CHECK(backend.get(s, "K") == "v1");

    // A second name takes the plain target; the first moves to its compound one.
    backend.set(s, "K2", "v2");
    CHECK(read_raw(s)->user == L"K2");
    const auto moved = read_raw(service.compound("K"));
    REQUIRE(moved.has_value());
    CHECK(moved->user == L"K");
    CHECK(moved->blob == utf16le(L"v1"));
    CHECK(backend.get(s, "K") == "v1");
    CHECK(backend.get(s, "K2") == "v2");

    // Overwriting the holder leaves its previous value at its compound target, as keyring does.
    backend.set(s, "K2", "v3");
    CHECK(backend.get(s, "K2") == "v3");
    CHECK(read_raw(service.compound("K2"))->blob == utf16le(L"v2"));

    CHECK(backend.remove(s, "K2"));
    CHECK_FALSE(read_raw(s).has_value());
    CHECK_FALSE(read_raw(service.compound("K2")).has_value());
    CHECK_FALSE(backend.get(s, "K2").has_value());
    CHECK(backend.get(s, "K") == "v1");
    CHECK(backend.remove(s, "K"));
    CHECK_FALSE(backend.remove(s, "K"));
    CHECK_FALSE(backend.get(s, "K").has_value());

    backend.set(s, "K", "é\U0001F600");
    CHECK(read_raw(s)->blob == utf16le(L"é\U0001F600"));
    CHECK(backend.get(s, "K") == "é\U0001F600");

    // A NUL in a name would cut the compound target short ("K" alone, outside the service):
    // refused before any call, as ctypes refuses it for keyring.
    const std::string with_nul("K\0X", 3);
    CHECK_THROWS_AS(backend.get(s, with_nul), std::invalid_argument);
    CHECK_THROWS_AS(backend.set(s, with_nul, "v"), std::invalid_argument);
    CHECK_THROWS_AS(backend.remove(s, with_nul), std::invalid_argument);
    CHECK_THROWS_AS(backend.get(s + std::string(1, '\0'), "K"), std::invalid_argument);
    CHECK(read_raw(s)->user == L"K");

    // A value is written with its size, so a NUL inside survives, as it does through keyring.
    const std::string value_with_nul("a\0b", 3);
    backend.set(s, "K", value_with_nul);
    CHECK(backend.get(s, "K") == value_with_nul);
}

TEST_CASE("WinCredBackend reads UTF-8 blobs and moves an entry without a user name", "[platform]") {
    TestService service;
    WinCredBackend backend;
    const std::string& s = service.name();

    write_raw(s, L"K", "abc"); // three bytes: not UTF-16
    CHECK(backend.get(s, "K") == "abc");

    write_raw(s, nullptr, utf16le(L"anonymous"));
    CHECK_FALSE(backend.get(s, "K").has_value());
    backend.set(s, "K", "mine");
    const auto moved = read_raw(service.compound("None"));
    REQUIRE(moved.has_value());
    CHECK_FALSE(moved->user.has_value());
    CHECK(moved->blob == utf16le(L"anonymous"));
    CHECK(backend.get(s, "K") == "mine");
}

TEST_CASE("CredentialStore over Credential Manager keeps a key under its service", "[platform]") {
    TestService service;
    const fv::AppIdentity identity{.product = "OpenWhisper_CPP",
                                   .credential_service = service.name()};
    fv::CredentialStore store(fv::make_platform_backend(), identity);
    CHECK(store.status().available);
    CHECK_FALSE(store.get("K").has_value());
    store.set("K", " sk-platform-test ");
    CHECK(store.get("K") == "sk-platform-test");
    CHECK(read_raw(service.name())->user == L"K");
    CHECK(store.remove("K"));
    CHECK_FALSE(store.remove("K"));
}

TEST_CASE("keys written by Python keyring read here, and the reverse", "[platform]") {
    // Python keyring 25.7 from OpenWhisper's environment, run with -I -B from a scratch folder.
    const auto python = fv::testkit::get_env("OPENWHISPER_CPP_OW_PYTHON");
    if (!python.has_value() || python->empty()) {
        SKIP("OPENWHISPER_CPP_OW_PYTHON is not set");
    }
    TestService service;
    TempDir temp;
    const auto script = temp.path() / "keyring_round_trip.py";
    write_bytes(script, R"(import sys
from keyring.backends.Windows import WinVaultKeyring
ring = WinVaultKeyring()
service, action = sys.argv[1], sys.argv[2]
if action == "write":
    ring.set_password(service, "PY", "from-python")
else:
    print(ring.get_password(service, "PY"), ring.get_password(service, "K"), sep="|")
)");
    const auto run = [&](std::string_view action) {
        const std::string command = std::format("\"\"{}\" -I -B \"{}\" {} {}\"", *python,
                                                fv::path_to_utf8(script), service.name(), action);
        std::string output;
        FILE* pipe = _popen(command.c_str(), "r");
        REQUIRE(pipe != nullptr);
        char buffer[256];
        while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
            output += buffer;
        }
        REQUIRE(_pclose(pipe) == 0);
        return output;
    };

    run("write");
    WinCredBackend backend;
    CHECK(backend.get(service.name(), "PY") == "from-python");
    backend.set(service.name(), "K", "from-openwhisper-cpp");
    CHECK(run("read") == "from-python|from-openwhisper-cpp\n");
}

#endif
