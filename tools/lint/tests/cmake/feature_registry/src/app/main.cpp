// Prints what the generated registry holds, in its order, and checks the embedded bytes.
#include <cstdio>
#include <string>
#include <string_view>

#include "appkit/feature.h"
#include "ui_alpha/feature.h"

int main() {
    for (const auto& factory : fv::registered_features()) {
        const auto feature = factory.make();
        std::printf("%s=%s ", std::string(factory.id).c_str(), std::string(feature->id()).c_str());
    }
    const std::string_view expected =
        "{\"text\": \"ciao caf\xc3\xa9\", \"lines\": [\"one\", \"two\", \"three\", \"four\", "
        "\"five\", \"six\"]}\n";
    std::printf("| embedded %s\n", fv::alpha_greeting() == expected ? "ok" : "differs");
    return 0;
}
