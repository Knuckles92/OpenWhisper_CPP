#pragma once

// What the paired-computer row tests share: the fixture meetings of
// tools/parity/cases/meeting.persist.repository_origin.py written as their rows, and the dumps
// those vectors compare.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "base/json.h"
#include "db/database.h"
#include "meeting_state/origin_rows.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fv::meeting_state::origin_test {

inline constexpr std::string_view kChildKeys[] = {"chunks",      "segments",  "participants",
                                                  "state_items", "questions", "events"};

inline std::string table_of(std::string_view key) {
    if (key == "session") {
        return "meeting_sessions";
    }
    if (key == "chunks") {
        return "meeting_audio_chunks";
    }
    return "meeting_" + std::string(key);
}

// {"$b64": text} as bytes; any other value as SQLite takes it.
inline sql::Value value_of(const Json& value) {
    if (value.is_object() && value.size() == 1 && value.contains("$b64")) {
        const auto bytes = b64decode(value.at("$b64").get<std::string>());
        if (!bytes) {
            throw std::runtime_error("bad $b64 in a fixture row");
        }
        return sql::Blob(bytes->begin(), bytes->end());
    }
    switch (value.type()) {
    case Json::value_t::null:
        return nullptr;
    case Json::value_t::boolean:
        return std::int64_t{value.get<bool>() ? 1 : 0};
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return value.get<std::int64_t>();
    case Json::value_t::number_float:
        return value.get<double>();
    case Json::value_t::string:
        return value.get<std::string>();
    default:
        throw std::runtime_error("unsupported fixture value " + value.dump());
    }
}

// One full row, every key a column.
inline void insert(sql::Connection& connection, std::string_view key, const Json& row) {
    std::string names;
    std::string params;
    std::vector<sql::Value> values;
    for (const auto& [name, value] : row.items()) {
        names += names.empty() ? "" : ", ";
        names += name;
        params += params.empty() ? "?" : ", ?";
        values.push_back(value_of(value));
    }
    sql::Statement statement = connection.prepare("INSERT INTO " + table_of(key) + " (" + names +
                                                  ") VALUES (" + params + ")");
    int index = 1;
    for (const sql::Value& value : values) {
        statement.bind(index++, value);
    }
    statement.step();
}

// The case file's `setup`: each meeting's session, then its child rows in _KEYS order.
inline void write_setup(Database& database, const Json& setup) {
    database.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        for (const Json& meeting : setup) {
            insert(connection, "session", meeting.at("session"));
            for (const std::string_view key : kChildKeys) {
                if (const auto rows = meeting.find(std::string(key)); rows != meeting.end()) {
                    for (const Json& row : *rows) {
                        insert(connection, key, row);
                    }
                }
            }
        }
        transaction.commit();
    });
}

// Bytes as the case file writes them, {"$b64": ...}.
inline Json b64_tagged(const Json& value) {
    if (value.is_binary()) {
        const auto& bytes = value.get_binary();
        std::vector<std::byte> raw;
        raw.reserve(bytes.size());
        for (const std::uint8_t byte : bytes) {
            raw.push_back(static_cast<std::byte>(byte));
        }
        return Json{{"$b64", b64encode(std::span<const std::byte>(raw))}};
    }
    if (value.is_object()) {
        Json copy = Json::object();
        for (const auto& [key, item] : value.items()) {
            copy[key] = b64_tagged(item);
        }
        return copy;
    }
    if (value.is_array()) {
        Json copy = Json::array();
        for (const Json& item : value) {
            copy.push_back(b64_tagged(item));
        }
        return copy;
    }
    return value;
}

// The `{"$b64": ...}` values of a row as Json::binary, as kinds.py's _row_from_json hands them on.
inline Json b64_untagged(const Json& value) {
    if (value.is_object() && value.size() == 1 && value.contains("$b64")) {
        const auto bytes = b64decode(value.at("$b64").get<std::string>());
        Json::binary_t binary;
        for (const std::byte byte : bytes.value()) {
            binary.push_back(static_cast<std::uint8_t>(byte));
        }
        return Json::binary(std::move(binary));
    }
    if (value.is_object()) {
        Json copy = Json::object();
        for (const auto& [key, item] : value.items()) {
            copy[key] = b64_untagged(item);
        }
        return copy;
    }
    if (value.is_array()) {
        Json copy = Json::array();
        for (const Json& item : value) {
            copy.push_back(b64_untagged(item));
        }
        return copy;
    }
    return value;
}

inline std::vector<Json> rows_of(Database& database, const std::string& sql) {
    return database.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(sql);
        std::vector<Json> rows;
        while (statement.step()) {
            Json row = Json::array();
            for (int index = 0; index < statement.column_count(); ++index) {
                const sql::Value value = statement.column(index);
                if (const auto* text = std::get_if<std::string>(&value)) {
                    row.push_back(*text);
                } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                    row.push_back(*integer);
                } else if (const auto* real = std::get_if<double>(&value)) {
                    row.push_back(*real);
                } else {
                    row.push_back(nullptr);
                }
            }
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

// The case file's _dump: every meeting's rows by id, the delivery order, and the segments the
// full-text index finds for "budget".
inline Json dump(Database& database) {
    Json meetings = Json::object();
    for (const Json& row : rows_of(database, "SELECT id FROM meeting_sessions ORDER BY id")) {
        const std::string id = row.at(0).get<std::string>();
        meetings[id] = b64_tagged(export_meeting_rows(database, id));
    }
    Json delivery = Json::array();
    for (const Json& row : rows_of(database, "SELECT segment_id, meeting_id FROM "
                                             "meeting_segment_delivery ORDER BY seq")) {
        delivery.push_back(row);
    }
    Json fts = Json::array();
    for (const Json& row : rows_of(
             database, "SELECT meeting_segments.id FROM meeting_segments_fts JOIN "
                       "meeting_segments ON meeting_segments.rowid = meeting_segments_fts.rowid "
                       "WHERE meeting_segments_fts MATCH 'budget' ORDER BY meeting_segments.id")) {
        fts.push_back(row.at(0));
    }
    return Json{{"meetings", meetings}, {"delivery", delivery}, {"fts", fts}};
}

// A fresh database in a temporary data folder, holding `setup`.
struct OriginDatabase {
    explicit OriginDatabase(const Json& setup = Json::array())
        : database(
              std::make_unique<Database>(Database::open_or_create(root.path() / "origin.db"))) {
        write_setup(*database, setup);
    }

    testkit::TempDataRoot root; // declared first, so the database closes before it is removed
    std::unique_ptr<Database> database;
};

} // namespace fv::meeting_state::origin_test
