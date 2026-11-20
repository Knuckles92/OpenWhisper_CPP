#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include "sql/db.h"

namespace fv {

// OpenWhisper's schema v18 unchanged, plus fv_* tables. WAL; one writer connection behind a
// mutex, pooled readers.
//
// Every connection has OpenWhisper's pragmas: foreign_keys=ON, journal_mode=WAL,
// synchronous=NORMAL, and a 30 s busy timeout. Opening runs services/database.py's
// DatabaseManager start-up: migrations of an older file, create_all, the legacy meeting tables
// dropped, the meeting FTS index and delivery trigger, the schema_version row, and the import of
// a transcription_history.json beside the database file.
//
// Affinity: thread-safe; blocking, never on the GUI thread.
class Database {
public:
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // The folder must exist. Throws sql::Error when the file cannot be opened or a migration
    // fails; nothing of a failed start-up is kept (it runs in one transaction).
    static Database open_or_create(const std::filesystem::path&); // migrates to v18
    // A database not opened yet, as services/database.py's _LazyDatabaseManager: nothing touches
    // the file until ensure_open() or the first read or write, which runs open_or_create's
    // start-up on its thread. A start-up that fails leaves it unopened and throws its error, and
    // the next use tries again, so an app whose database will not open keeps running with
    // failing saves. Does not block; may be made on the GUI thread.
    static Database deferred(const std::filesystem::path&);

    // Opens a deferred database now (_LazyDatabaseManager.ensure_initialized); nothing when it
    // is open. Throws sql::Error as open_or_create does, or when closed.
    // Blocking: fails in a debug build when called on the GUI thread.
    void ensure_open();
    // Whether the start-up has run (and close() has not).
    bool is_open() const;

    // fn(sql::Connection&) under the writer mutex. The mutex is recursive, so a write may call
    // write again on the same thread. fn opens its own transaction (Connection::begin).
    // Blocking: fails in a debug build when called on the GUI thread (architecture 4, rule 2).
    template <class F> auto write(F&& fn) {
        WriterLock lock = lock_writer();
        return std::forward<F>(fn)(*lock.connection);
    }
    // fn(sql::Connection&) on a reader that this thread holds alone until fn returns. Readers are
    // pooled: one is opened when every pooled one is in use, and goes back to the pool afterwards,
    // so threads that come and go do not pile up connections, and close() never closes a reader
    // that fn is still using. fn must not keep the connection or its statements past its return.
    // Readers are query_only: a write through one fails with SQLITE_READONLY.
    // Blocking: fails in a debug build when called on the GUI thread (architecture 4, rule 2).
    template <class F> auto read(F&& fn) {
        ReaderLease lease = lease_reader();
        return std::forward<F>(fn)(*lease.connection);
    }

    int schema_version() const; // 18 (the version the file will have, until it is opened)
    const std::filesystem::path& path() const;

    // Closes the writer and every pooled reader (DatabaseManager.close). Call it once nothing
    // reads or writes any more (after the persistence strand has drained): it waits for a write
    // in progress, and a read in progress closes its reader when it ends. Later reads and writes
    // throw sql::Error. Idempotent.
    void close();

private:
    struct Impl;

    struct WriterLock {
        std::unique_lock<std::recursive_mutex> lock;
        sql::Connection* connection;
    };

    // A pooled reader, handed back to the pool (or closed, once the database is) on
    // destruction.
    struct ReaderLease {
        ReaderLease(Impl* impl, std::unique_ptr<sql::Connection> connection);
        ReaderLease(ReaderLease&&) = delete;
        ReaderLease& operator=(ReaderLease&&) = delete;
        ~ReaderLease();

        Impl* impl;
        std::unique_ptr<sql::Connection> connection;
    };

    explicit Database(std::unique_ptr<Impl>);
    // ensure_open with the writer mutex held.
    void open_locked();
    WriterLock lock_writer();
    ReaderLease lease_reader();

    std::unique_ptr<Impl> impl_;
};

} // namespace fv
