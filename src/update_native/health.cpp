#include "update_native/health.h"
#include "base/hash.h"
#include "base/hmac.h"
#include "update/update_contract.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include <algorithm>

namespace fv::update_native {
std::optional<HealthRequest> health_request(std::span<const std::string> args) {
    if (std::ranges::find(args, update::kHealthArg) == args.end())
        return {};
    if (args.size() != 4 || args[0] != update::kHealthArg || args[2] != update::kTransactionArg) {
        throw UpdateError("The update health request is invalid.");
    }
    const auto& token = args[1];
    if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw UpdateError("The update health request is invalid.");
    }
    return HealthRequest{update::validate_transaction_id(args[3]), token};
}

bool check_health(const JournalContext& context, const HealthRequest& request,
                  const std::filesystem::path& executable, std::string_view version,
                  bool acknowledge) {
    try {
        const auto& identity = update::release_identity();
        const auto expected = context.install_dir / identity.app_exe;
        if (executable.filename() != expected.filename() ||
            !std::filesystem::equivalent(executable, expected))
            return false;
        auto paths = journal_paths(context, request.transaction_id);
        GuardedDirectory install(paths.install_dir);
        GuardedDirectory transaction(paths.transaction_dir);
        const auto journal = load_journal(context, request.transaction_id);
        if (journal.state != TransactionState::NewActive || journal.new_version != version ||
            !constant_time_equal(journal.health_token, request.token))
            return false;
        // The executable stays mapped by this process. The guard opens its single-link regular
        // file without write or delete sharing and rejects linked parent directories.
        constexpr std::size_t max_image = 256u * 1024u * 1024u;
        if (sha256_hex(install.read_file(expected.filename(), max_image)) != journal.new_exe_sha256)
            return false;
        if (acknowledge)
            transaction.write_file_atomic(update::kHealthName, request.token);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace fv::update_native
