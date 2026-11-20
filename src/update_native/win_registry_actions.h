#pragma once
#ifdef _WIN32
#include "update_native/native_platform.h"

namespace fv::update_native {
// Context must be constructed independently from registry discovery and trusted data root.
class WinRegistryActions final : public RegistryActions {
public:
    WinRegistryActions(JournalContext, std::string_view transaction_id);
    void verify(const UpdateJournal&) override;
    void install_recovery(const UpdateJournal&) override;
    void clear_recovery(const UpdateJournal&) override;
    void update(const UpdateJournal&) override;
    void restore(const UpdateJournal&) override;

private:
    void write_version(const UpdateJournal&, bool rollback);
    JournalContext context_;
    JournalPaths paths_;
    std::wstring command_;
};
} // namespace fv::update_native
#endif
