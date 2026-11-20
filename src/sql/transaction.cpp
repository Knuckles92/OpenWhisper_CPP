#include <utility>

#include "sql/db.h"

namespace fv::sql {

Transaction::Transaction(Transaction&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)) {}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        if (connection_ != nullptr) {
            try {
                rollback();
            } catch (...) {
                // The connection is closing or already outside a transaction.
            }
        }
        connection_ = std::exchange(other.connection_, nullptr);
    }
    return *this;
}

Transaction::~Transaction() {
    if (connection_ != nullptr) {
        try {
            rollback();
        } catch (...) {
            // A failed ROLLBACK leaves SQLite's own automatic rollback; nothing more can be done
            // from a destructor.
        }
    }
}

void Transaction::commit() {
    if (connection_ == nullptr) {
        throw Error(21, "the transaction is already finished"); // SQLITE_MISUSE
    }
    // On failure (SQLITE_BUSY) the transaction stays open, so the caller may retry or roll back.
    connection_->exec("COMMIT");
    connection_ = nullptr;
}

void Transaction::rollback() {
    if (connection_ == nullptr) {
        return;
    }
    Connection* connection = std::exchange(connection_, nullptr);
    // SQLite may already have rolled back after an error such as SQLITE_FULL.
    if (!connection->autocommit()) {
        connection->exec("ROLLBACK");
    }
}

} // namespace fv::sql
