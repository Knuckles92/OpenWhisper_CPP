#include "db/database.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "base/executor.h"
#include "base/log.h"
#include "base/utf8.h"
#include "db/detail/startup.h"
#include "db/schema.h"

namespace fv {

namespace {

const log::Logger logger{"services.database"};

constexpr int sqlite_misuse = 21;

// config.HISTORY_FILE sits beside config.DATABASE_FILE in the data root.
constexpr std::string_view history_file_name = "transcription_history.json";

[[noreturn]] void throw_closed() {
    throw sql::Error(sqlite_misuse, "The database is closed.");
}

} // namespace

struct Database::Impl {
    std::filesystem::path path;
    int version = schema::schema_version;

    std::recursive_mutex writer_mutex;
    std::optional<sql::Connection> writer; // empty until opened, and once closed
    // Set under writer_mutex once the start-up has run; read without it by lease_reader.
    std::atomic<bool> opened{false};

    std::mutex readers_mutex;
    bool closed = false;
    // `closed`, readable under writer_mutex too: a deferred database closed before it opened
    // must not open afterwards.
    std::atomic<bool> closed_flag{false};
    // Readers no thread is using. A lease takes the most recently returned one, so a thread that
    // reads again and again keeps one connection (and its statement cache warm).
    std::vector<std::unique_ptr<sql::Connection>> idle_readers;
};

Database::Database(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Database::Database(Database&&) noexcept = default;

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Database::~Database() {
    close();
}

Database Database::open_or_create(const std::filesystem::path& path) {
    Database database = deferred(path);
    database.ensure_open();
    return database;
}

Database Database::deferred(const std::filesystem::path& path) {
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    return Database(std::move(impl));
}

void Database::ensure_open() {
    assert_not_gui_thread();
    if (!impl_) {
        throw_closed();
    }
    if (impl_->opened.load()) {
        return;
    }
    const std::lock_guard lock(impl_->writer_mutex);
    open_locked();
}

bool Database::is_open() const {
    return impl_ != nullptr && impl_->opened.load() && !impl_->closed_flag.load();
}

void Database::open_locked() {
    if (impl_->opened.load()) {
        if (!impl_->writer) {
            throw_closed();
        }
        return;
    }
    if (impl_->closed_flag.load()) {
        throw_closed();
    }
    const std::filesystem::path& path = impl_->path;
    sql::Connection writer = sql::Connection::open(path, sql::OpenMode::Create);
    db_detail::configure_connection(writer, false);
    db_detail::init_database(writer);
    db_detail::migrate_from_json(writer, path.parent_path() / history_file_name);

    const sql::Value stored = writer.scalar("SELECT version FROM schema_version LIMIT 1");
    if (const auto* version = std::get_if<std::int64_t>(&stored)) {
        impl_->version = static_cast<int>(*version);
    }
    impl_->writer.emplace(std::move(writer));
    impl_->opened = true;
    logger.info("DatabaseManager initialized: {}", path_to_utf8(path));
}

int Database::schema_version() const {
    if (!impl_) {
        throw_closed();
    }
    return impl_->version;
}

const std::filesystem::path& Database::path() const {
    if (!impl_) {
        throw_closed();
    }
    return impl_->path;
}

Database::WriterLock Database::lock_writer() {
    assert_not_gui_thread();
    if (!impl_) {
        throw_closed();
    }
    std::unique_lock lock(impl_->writer_mutex);
    if (!impl_->writer) {
        // Deferred: the first write runs the start-up (or throws its error); closed: throws.
        open_locked();
    }
    return WriterLock{std::move(lock), &*impl_->writer};
}

Database::ReaderLease::ReaderLease(Impl* owner, std::unique_ptr<sql::Connection> reader)
    : impl(owner), connection(std::move(reader)) {}

Database::ReaderLease::~ReaderLease() {
    // A reader left inside a transaction would pin an old snapshot for the next lease; close it
    // instead of handing it back.
    bool reusable = false;
    try {
        reusable = connection->is_open() && connection->autocommit();
    } catch (...) {
    }
    std::unique_ptr<sql::Connection> closing;
    {
        const std::lock_guard lock(impl->readers_mutex);
        if (reusable && !impl->closed) {
            try {
                impl->idle_readers.push_back(std::move(connection));
            } catch (...) {
                // Out of memory: push_back left the connection here, and it is closed below.
            }
        }
        closing = std::move(connection);
    }
    // Closed outside the lock: sqlite3_close may wait on the file system.
}

Database::ReaderLease Database::lease_reader() {
    assert_not_gui_thread();
    if (!impl_) {
        throw_closed();
    }
    if (!impl_->opened.load()) {
        ensure_open();
    }
    {
        const std::lock_guard lock(impl_->readers_mutex);
        if (impl_->closed) {
            throw_closed();
        }
        if (!impl_->idle_readers.empty()) {
            std::unique_ptr<sql::Connection> reader = std::move(impl_->idle_readers.back());
            impl_->idle_readers.pop_back();
            return ReaderLease(impl_.get(), std::move(reader));
        }
    }
    // Opened outside the lock, so other threads keep leasing pooled readers meanwhile.
    auto reader = std::make_unique<sql::Connection>(
        sql::Connection::open(impl_->path, sql::OpenMode::ReadWrite));
    db_detail::configure_connection(*reader, true);
    return ReaderLease(impl_.get(), std::move(reader));
}

void Database::close() {
    if (!impl_) {
        return;
    }
    std::vector<std::unique_ptr<sql::Connection>> readers;
    {
        const std::lock_guard lock(impl_->readers_mutex);
        impl_->closed = true;
        impl_->closed_flag = true;
        readers.swap(impl_->idle_readers);
    }
    readers.clear();
    const std::lock_guard lock(impl_->writer_mutex);
    impl_->writer.reset();
}

} // namespace fv
