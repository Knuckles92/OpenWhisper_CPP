#include <memory>
#include <string>

#include "credentials/platform_backend.h"

namespace fv {

namespace {

constexpr std::string_view kNotSupported = "not supported on this platform";

// No Keychain or Secret Service backend yet: the store reports itself unavailable, so keys come
// from the environment and .env only, and saving explains why it cannot.
class UnavailableBackend final : public CredentialBackend {
public:
    std::optional<std::string> get(std::string_view, std::string_view) override { fail(); }
    void set(std::string_view, std::string_view, std::string_view) override { fail(); }
    bool remove(std::string_view, std::string_view) override { fail(); }
    CredentialStatus probe() override {
        return {false, std::string(platform_backend_name()), std::string(kNotSupported)};
    }

private:
    [[noreturn]] static void fail() {
        throw CredentialStoreError(std::string(platform_backend_name()) + " is " +
                                   std::string(kNotSupported));
    }
};

} // namespace

std::shared_ptr<CredentialBackend> make_platform_backend() {
    return std::make_shared<UnavailableBackend>();
}

} // namespace fv
