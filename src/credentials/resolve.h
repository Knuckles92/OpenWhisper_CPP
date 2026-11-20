#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/env.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"

namespace fv {

// Which layer resolve_credential would answer from. An environment variable holding exactly the
// .env value counts as the file's (an editor such as VS Code may have loaded .env into the
// environment of the process it started).
CredentialSource credential_source(CredentialStore&, const EnvReader&, const DotEnv&,
                                   std::string_view name);

// True when a saved key hides a value the environment or .env also provides.
bool environment_shadowed(CredentialStore&, const EnvReader&, const DotEnv&, std::string_view name);

// OpenWhisper's spelling of a source: "stored", "environment", "dotenv", "none".
std::string_view credential_source_name(CredentialSource);

// The .env value for a credential name, as a lookup by that name sees it: on Windows, where
// environment names ignore case, the first entry with a value whose name matches in any case;
// elsewhere the entry of exactly that name.
std::optional<std::string> dotenv_value(const DotEnv&, std::string_view name);

} // namespace fv
