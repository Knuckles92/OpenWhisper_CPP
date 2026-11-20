#include "remote_records/outbox.h"

#include <chrono>
#include <format>

namespace fv::remote_records {
namespace {

std::string now(Clock& clock) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(clock.utc()));
}

std::string text(const sql::Value& value) {
    const auto* found = std::get_if<std::string>(&value);
    return found ? *found : std::string();
}

} // namespace

bool Outbox::enqueue(const OutboxItem& item) {
    return db_.write([&](sql::Connection& connection) {
        auto old = connection.prepare(
            "SELECT host_fingerprint,action,state,content_digest FROM record_sync "
            "WHERE kind=? AND record_id=?");
        old.bind(1, item.kind).bind(2, item.id);
        if (old.step() && text(old.column(0)) == item.host && text(old.column(1)) == item.action &&
            text(old.column(2)) == "done" && text(old.column(3)) == item.digest) {
            return false;
        }
        const auto stamp = now(clock_);
        auto write = connection.prepare(
            "INSERT INTO record_sync "
            "(kind,record_id,host_fingerprint,action,state,attempts,last_error,content_digest,"
            "created_at,updated_at) VALUES (?,?,?,?,'pending',0,NULL,?,?,?) "
            "ON CONFLICT(kind,record_id) DO UPDATE SET host_fingerprint=excluded.host_fingerprint,"
            "action=excluded.action,state='pending',attempts=0,last_error=NULL,"
            "content_digest=excluded.content_digest,updated_at=excluded.updated_at");
        write.bind(1, item.kind)
            .bind(2, item.id)
            .bind(3, item.host)
            .bind(4, item.action)
            .bind(5, item.digest)
            .bind(6, stamp)
            .bind(7, stamp);
        (void)write.step();
        return true;
    });
}

std::vector<OutboxItem> Outbox::rows(std::string_view host) {
    return db_.read([&](sql::Connection& connection) {
        auto query = connection.prepare(
            "SELECT kind,record_id,host_fingerprint,action,state,content_digest FROM record_sync "
            "WHERE host_fingerprint=? ORDER BY created_at,kind,record_id");
        query.bind(1, std::string(host));
        std::vector<OutboxItem> out;
        while (query.step()) {
            out.push_back({text(query.column(0)), text(query.column(1)), text(query.column(2)),
                           text(query.column(3)), text(query.column(4)), text(query.column(5))});
        }
        return out;
    });
}

void Outbox::complete(const OutboxItem& item) {
    db_.write([&](sql::Connection& connection) {
        auto query = connection.prepare(
            "UPDATE record_sync SET state='done',attempts=0,last_error=NULL,updated_at=? "
            "WHERE kind=? AND record_id=? AND host_fingerprint=? AND content_digest=? AND "
            "action=?");
        query.bind(1, now(clock_))
            .bind(2, item.kind)
            .bind(3, item.id)
            .bind(4, item.host)
            .bind(5, item.digest)
            .bind(6, item.action);
        (void)query.step();
    });
}

void Outbox::fail(const OutboxItem& item, std::string_view reason) {
    db_.write([&](sql::Connection& connection) {
        auto query = connection.prepare(
            "UPDATE record_sync SET state='failed',attempts=attempts+1,last_error=?,updated_at=? "
            "WHERE kind=? AND record_id=? AND host_fingerprint=? AND content_digest=? AND "
            "action=?");
        query.bind(1, std::string(reason))
            .bind(2, now(clock_))
            .bind(3, item.kind)
            .bind(4, item.id)
            .bind(5, item.host)
            .bind(6, item.digest)
            .bind(7, item.action);
        (void)query.step();
    });
}

void Outbox::forget(std::string_view kind, std::string_view id, std::string_view host) {
    db_.write([&](sql::Connection& connection) {
        auto query = connection.prepare(
            "DELETE FROM record_sync WHERE kind=? AND record_id=? AND host_fingerprint=?");
        query.bind(1, std::string(kind)).bind(2, std::string(id)).bind(3, std::string(host));
        (void)query.step();
    });
}

void Outbox::mark_done(const OutboxItem& item) {
    db_.write([&](sql::Connection& connection) {
        const auto stamp = now(clock_);
        auto write = connection.prepare(
            "INSERT INTO record_sync "
            "(kind,record_id,host_fingerprint,action,state,attempts,last_error,content_digest,"
            "created_at,updated_at) VALUES (?,?,?,?,'done',0,NULL,?,?,?) "
            "ON CONFLICT(kind,record_id) DO UPDATE SET host_fingerprint=excluded.host_fingerprint,"
            "action=excluded.action,state='done',attempts=0,last_error=NULL,"
            "content_digest=excluded.content_digest,updated_at=excluded.updated_at");
        write.bind(1, item.kind)
            .bind(2, item.id)
            .bind(3, item.host)
            .bind(4, item.action)
            .bind(5, item.digest)
            .bind(6, stamp)
            .bind(7, stamp);
        (void)write.step();
    });
}

void Outbox::retarget(std::string_view location) {
    db_.write([&](sql::Connection& connection) {
        if (location == "local") {
            auto drop = connection.prepare("DELETE FROM record_sync WHERE state IN "
                                           "('pending','failed') AND action<>'delete'");
            (void)drop.step();
            return;
        }
        auto change = connection.prepare(
            "UPDATE record_sync SET action=?,updated_at=? WHERE state IN ('pending','failed') "
            "AND action<>'delete'");
        change.bind(1, std::string(location == "host" ? "move" : "copy")).bind(2, now(clock_));
        (void)change.step();
    });
}

} // namespace fv::remote_records
