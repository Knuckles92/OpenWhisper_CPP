#include "update_native/health_process.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "download/verified_files.h"
#include "update/release_identity.h"
#include "update/update_contract.h"
#include "update_native/detail/health_child.h"

namespace fv::update_native {
struct HealthProcess::Impl {
    std::unique_ptr<detail::HealthChild> child;
};
HealthProcess::HealthProcess() : impl_(std::make_unique<Impl>()) {}
HealthProcess::~HealthProcess() = default;

bool HealthProcess::launch_and_wait(TransactionStorage& storage, std::string_view token,
                                    std::string_view expected_sha256,
                                    std::chrono::milliseconds timeout, const CancelToken& cancel) {
    if (impl_->child || token.size() != 32 || expected_sha256.size() != 64 || timeout.count() < 0 ||
        storage.has_data_lease() || !std::all_of(token.begin(), token.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw std::invalid_argument("Invalid health launch state.");
    const auto& paths = storage.paths();
    const auto app = paths.install_dir / path_from_utf8(update::release_identity().app_exe);
    const auto id = path_to_utf8(paths.transaction_dir.filename());
    GuardedDirectory install(paths.install_dir);
    auto pinned_executable = install.pin_file(app.filename());
    if (!pinned_executable)
        throw std::runtime_error("The updated application is missing.");
    if (download::sha256_file(app, true) != expected_sha256)
        throw std::runtime_error("The updated application failed verification.");
    storage.transaction_directory().remove_file(update::kHealthName);
    impl_->child = detail::HealthChild::launch(app,
                                               {std::string(update::kHealthArg), std::string(token),
                                                std::string(update::kTransactionArg), id},
                                               paths.install_dir);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        try {
            if (storage.transaction_directory().read_file(update::kHealthName, 32) == token)
                return true;
        } catch (const std::exception&) {
            // The health file is published atomically; until then it may not exist.
        }
        if (cancel.cancelled())
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        // A new version that exits has failed: there is nothing left to wait for.
        if (impl_->child->wait(
                std::min(left + std::chrono::milliseconds(1), std::chrono::milliseconds(100))))
            return false;
    }
}
void HealthProcess::stop() {
    if (impl_->child)
        impl_->child->terminate();
}
} // namespace fv::update_native
