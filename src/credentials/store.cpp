#include "credentials/store.h"

#include <new>
#include <system_error>
#include <utility>

#include "base/executor.h"
#include "base/log.h"
#include "credentials/detail/errors.h"
#include "credentials/platform_backend.h"
#include "credentials/validate.h"
#include "pyport/errors.h"

namespace fv {

namespace credentials_detail {

std::string_view exception_type_name(const std::exception& error) {
    // Most derived first.
    if (dynamic_cast<const CredentialStoreTimeout*>(&error) != nullptr) {
        return "CredentialStoreTimeout";
    }
    if (dynamic_cast<const CredentialStoreError*>(&error) != nullptr) {
        return "CredentialStoreError";
    }
    if (dynamic_cast<const BlobDecodeError*>(&error) != nullptr) {
        return "UnicodeDecodeError";
    }
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return python->type_name();
    }
    if (dynamic_cast<const std::system_error*>(&error) != nullptr) {
        return "system_error";
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return "invalid_argument";
    }
    if (dynamic_cast<const std::runtime_error*>(&error) != nullptr) {
        return "runtime_error";
    }
    if (dynamic_cast<const std::logic_error*>(&error) != nullptr) {
        return "logic_error";
    }
    if (dynamic_cast<const std::bad_alloc*>(&error) != nullptr) {
        return "bad_alloc";
    }
    return "exception";
}

} // namespace credentials_detail

namespace {

using credentials_detail::exception_type_name;

constexpr log::Logger logger("services.credentials");

// Runs `call`, turning anything that is not a std::exception into one, so a backend that throws
// something else is reported like any other failure.
template <class Call> auto call_backend(Call&& call) -> decltype(call()) {
    try {
        return call();
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        throw std::runtime_error("unknown exception");
    }
}

} // namespace

CredentialStore::CredentialStore(std::shared_ptr<CredentialBackend> backend,
                                 const AppIdentity& identity)
    : backend_(std::move(backend)), service_(identity.credential_service) {}

CredentialStatus CredentialStore::status() {
    assert_not_gui_thread();
    std::lock_guard lock(mutex_);
    return status_locked();
}

CredentialStatus CredentialStore::status_locked() {
    if (status_.has_value()) {
        return *status_;
    }
    if (backend_ == nullptr) {
        status_ = CredentialStatus{false, std::string(platform_backend_name()), "no backend"};
        return *status_;
    }
    try {
        status_ = call_backend([&] { return backend_->probe(); });
        if (!status_->available) {
            logger.warning("Credential store unavailable: {}", status_->reason);
        }
    } catch (const std::exception& error) {
        const std::string_view type = exception_type_name(error);
        // A probe is never given a secret, so its message may be logged and shown.
        const std::string message = error.what();
        logger.warning("Credential store unavailable: {}: {}", type, message);
        status_ = CredentialStatus{false, std::string(platform_backend_name()),
                                   message.empty() ? std::string(type) : message};
    }
    return *status_;
}

CredentialStatus CredentialStore::refresh() {
    assert_not_gui_thread();
    std::lock_guard lock(mutex_);
    status_.reset();
    cache_.clear();
    return status_locked();
}

void CredentialStore::require_backend_locked() {
    const CredentialStatus status = status_locked();
    if (!status.available) {
        throw CredentialStoreError(status.backend_name + " is not available: " + status.reason);
    }
}

void CredentialStore::mark_unresponsive_locked() {
    std::string name =
        status_.has_value() ? status_->backend_name : std::string(platform_backend_name());
    status_ = CredentialStatus{false, std::move(name), std::string(kKeyringNoResponse)};
}

std::optional<std::string> CredentialStore::get(std::string_view raw_name) {
    assert_not_gui_thread();
    const std::string name = validate_credential_name(raw_name);
    std::optional<std::string> value;
    std::string_view failure;
    bool timed_out = false;
    {
        std::lock_guard lock(mutex_);
        if (const auto cached = cache_.find(name); cached != cache_.end()) {
            return cached->second;
        }
        if (!status_locked().available) {
            return std::nullopt;
        }
        try {
            value = call_backend([&] { return backend_->get(service_, name); });
        } catch (const CredentialStoreTimeout&) {
            timed_out = true;
            mark_unresponsive_locked();
        } catch (const std::exception& error) {
            failure = exception_type_name(error);
        }
        // A failed read is remembered like a missing key: retrying on every read would put a
        // fresh access prompt up at each step. Saving or removing the key clears it.
        if (value.has_value() && value->empty()) {
            value.reset();
        }
        cache_[name] = value;
    }
    if (timed_out) {
        logger.warning("Reading {} from the credential store timed out: {}", name,
                       kKeyringNoResponse);
    } else if (!failure.empty()) {
        logger.warning("Reading {} from the credential store failed: {}", name, failure);
    }
    return value;
}

bool CredentialStore::has(std::string_view name) {
    return get(name).has_value();
}

void CredentialStore::request_access(std::string_view raw_name) {
    assert_not_gui_thread();
    const std::string name = validate_credential_name(raw_name);
    std::lock_guard lock(mutex_);
    cache_.erase(name);
    status_.reset();
    require_backend_locked();
    std::optional<std::string> value;
    try {
        value = call_backend([&] { return backend_->get(service_, name); });
    } catch (const std::exception& error) {
        cache_[name] = std::nullopt;
        throw CredentialStoreError("Couldn't access " + status_locked().backend_name + " (" +
                                   std::string(exception_type_name(error)) + ").");
    }
    if (value.has_value() && value->empty()) {
        value.reset();
    }
    cache_[name] = std::move(value);
}

void CredentialStore::set(std::string_view raw_name, std::string_view raw_value) {
    assert_not_gui_thread();
    const std::string name = validate_credential_name(raw_name);
    std::string value = validate_api_key(raw_value);
    std::string backend_name;
    {
        std::unique_lock lock(mutex_);
        require_backend_locked();
        try {
            call_backend([&] { backend_->set(service_, name, value); });
        } catch (const std::exception& error) {
            if (dynamic_cast<const CredentialStoreTimeout*>(&error) != nullptr) {
                mark_unresponsive_locked();
            }
            const std::string type(exception_type_name(error));
            backend_name = status_locked().backend_name;
            lock.unlock();
            logger.error("Saving {} to the credential store failed: {}", name, type);
            throw CredentialStoreError("Couldn't save the key to " + backend_name + " (" + type +
                                       ").");
        }
        cache_[name] = std::move(value);
        backend_name = status_locked().backend_name;
    }
    logger.info("Saved {} to {}", name, backend_name);
}

bool CredentialStore::remove(std::string_view raw_name) {
    assert_not_gui_thread();
    const std::string name = validate_credential_name(raw_name);
    std::string backend_name;
    {
        std::unique_lock lock(mutex_);
        require_backend_locked();
        bool removed = false;
        try {
            removed = call_backend([&] { return backend_->remove(service_, name); });
        } catch (const std::exception& error) {
            if (dynamic_cast<const CredentialStoreTimeout*>(&error) != nullptr) {
                mark_unresponsive_locked();
            }
            const std::string type(exception_type_name(error));
            backend_name = status_locked().backend_name;
            lock.unlock();
            logger.error("Removing {} from the credential store failed: {}", name, type);
            throw CredentialStoreError("Couldn't remove the key from " + backend_name + " (" +
                                       type + ").");
        }
        cache_[name] = std::nullopt;
        if (!removed) {
            return false;
        }
        backend_name = status_locked().backend_name;
    }
    logger.info("Removed {} from {}", name, backend_name);
    return true;
}

} // namespace fv
