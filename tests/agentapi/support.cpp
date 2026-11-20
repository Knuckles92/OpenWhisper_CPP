#include "support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

#include "base/utf8.h"
#include "db/database.h"
#include "pyport/json.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fv::agentapi::test {

namespace {

constexpr std::string_view kPrivate = "private-marker";

Json snapshot() {
    return py::loads(R"({
        "rolling_summary": "Launch plan agreed.",
        "rolling_summary_evidence": ["sg_a"],
        "participants": {"p_a": {"id": "p_a", "display_name": "Alex", "kind": "me",
                                 "token": "private-marker"}},
        "cards": {"action_items": [
            {"id": "it_a", "text": "Ship the launch", "status": "confirmed",
             "evidence": ["sg_a"],
             "data": {"owner_participant_id": "p_a", "private_path": "private-marker"}},
            {"id": "it_removed", "text": "Removed note", "status": "removed"}]},
        "questions": [{"id": "q_a", "text": "When?", "answer": "Friday", "status": "resolved",
                       "evidence": ["sg_b"]}],
        "custom_reports": [{"id": "r_a", "title": "Plan", "markdown": "Launch Friday",
                            "status": "ready", "sources": {"path": "private-marker"}}],
        "capture": {"path": "private-marker"}
    })");
}

void run(sql::Connection& connection, std::string_view sql, const std::vector<sql::Value>& values) {
    sql::Statement statement = connection.prepare(sql);
    for (std::size_t i = 0; i < values.size(); ++i) {
        statement.bind(static_cast<int>(i) + 1, values[i]);
    }
    while (statement.step()) {
    }
}

// Percent-decoding as the server does for paths and query values; `plus` reads '+' as a space.
std::string unquote(std::string_view text, bool plus) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            out += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else if (plus && text[i] == '+') {
            out += ' ';
        } else {
            out += text[i];
        }
    }
    return out;
}

sql::Value text_or_null(std::optional<std::string_view> value) {
    return value ? sql::Value(std::string(*value)) : sql::Value(nullptr);
}

} // namespace

net::HttpRequest request(std::string path, Pairs query,
                         std::vector<std::pair<std::string, std::optional<std::string>>> headers,
                         std::string method) {
    net::HttpRequest out;
    out.method = std::move(method);
    // A path may carry its query, as the search hits' resources do.
    const std::size_t mark = path.find('?');
    if (mark != std::string::npos) {
        std::string_view rest = std::string_view(path).substr(mark + 1);
        while (!rest.empty()) {
            const std::string_view pair = rest.substr(0, rest.find('&'));
            const std::size_t equals = pair.find('=');
            out.query.emplace_back(unquote(pair.substr(0, equals), true),
                                   equals == std::string_view::npos
                                       ? std::string()
                                       : unquote(pair.substr(equals + 1), true));
            rest.remove_prefix(std::min(rest.size(), pair.size() + 1));
        }
        path.resize(mark);
    }
    out.path = unquote(path, false);
    out.query.insert(out.query.end(), query.begin(), query.end());
    out.tls = false;
    out.peer = {"127.0.0.1", 50000};
    std::vector<std::pair<std::string, std::optional<std::string>>> all = {
        {"host", "127.0.0.1"}, {"authorization", "Bearer " + kToken}};
    for (auto& [name, value] : headers) {
        bool replaced = false;
        for (auto& existing : all) {
            std::string lower = name;
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (existing.first == lower) {
                existing.second = value;
                replaced = true;
            }
        }
        if (!replaced) {
            all.emplace_back(name, value);
        }
    }
    for (auto& [name, value] : all) {
        if (value) {
            out.headers.emplace_back(name, *value);
        }
    }
    return out;
}

std::optional<std::string> Reply::header(std::string_view name) const {
    for (const auto& [key, value] : response.headers) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

Reply get(HistoryApi& api, std::string path, Pairs query,
          std::vector<std::pair<std::string, std::optional<std::string>>> headers) {
    Reply reply;
    reply.response = api.handle(request(std::move(path), std::move(query), std::move(headers)));
    try {
        reply.json = py::loads(reply.response.body);
    } catch (const py::PyError&) {
        reply.json = nullptr;
    }
    return reply;
}

std::vector<Json> all_pages(HistoryApi& api, const std::string& path, Pairs query) {
    std::vector<Json> items;
    std::set<std::string> cursors;
    while (true) {
        const Reply reply = get(api, path, query);
        REQUIRE(reply.status() == 200);
        for (const Json& item : reply.json.at("items")) {
            items.push_back(item);
        }
        const Json& next = reply.json.at("next_cursor");
        if (!next.is_string() || next.get<std::string>().empty()) {
            return items;
        }
        REQUIRE(cursors.insert(next.get<std::string>()).second);
        std::erase_if(query, [](const auto& pair) { return pair.first == "cursor"; });
        query.emplace_back("cursor", next.get<std::string>());
    }
}

std::vector<std::string> ids(const std::vector<Json>& items) {
    std::vector<std::string> out;
    for (const Json& item : items) {
        out.push_back(item.at("id").get<std::string>());
    }
    return out;
}

std::filesystem::path copy_history_fixture(const std::filesystem::path& folder,
                                           std::string_view name) {
    const auto source = testkit::source_root() / "tests" / "fixtures" / "agentapi" / "history.db";
    const auto target = folder / path_from_utf8(name);
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    return target;
}

std::filesystem::path make_test_database(const std::filesystem::path& folder) {
    const auto path = folder / path_from_utf8("history # café.db");
    Database database = Database::open_or_create(path);
    database.write([](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        const std::string history =
            "INSERT INTO transcription_history (id, text, raw_text, timestamp, model, audio_file, "
            "source_name, origin_device_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
        using V = std::vector<sql::Value>;
        const sql::Value null = nullptr;
        run(connection, history,
            V{std::string("h_a"), std::string("The launch is Friday"),
              std::string("raw phrase only"), std::string(kDate), std::string("test"),
              std::string(kPrivate), std::string("plan.wav"), null});
        run(connection, history,
            V{std::string("h_b"), std::string("Literal 50%_quote ' OR 1=1 --"), null,
              std::string(kDate), std::string("test"), null, null, null});
        run(connection, history,
            V{std::string("h_c"), std::string(1000, 'x') + " Needle near end", null,
              std::string("2026-09-01T09:00:00-07:00"), std::string("test"), null, null, null});
        run(connection, history,
            V{std::string("h_old"), std::string("launch before"), null,
              std::string("2026-08-31T23:00:00"), std::string("test"), null, null, null});
        run(connection, history,
            V{std::string("h_remote"), std::string("launch remote"), null, std::string(kDate),
              std::string("test"), null, null, std::string("device_a")});
        const std::string state = py::dumps(snapshot());
        for (const auto& [id, origin] :
             std::vector<std::pair<std::string, std::optional<std::string_view>>>{
                 {"m_a", std::nullopt}, {"m_remote", "device_a"}, {"m_empty", std::nullopt}}) {
            run(connection,
                "INSERT INTO meeting_sessions (id, title, status, started_at, ended_at, "
                "paused_total_s, host_token, guest_token, cloud_enabled, spool_dir, state_json, "
                "state_seq, agent_endpoint_json, origin_device_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, "
                "?, ?, ?, ?, ?, ?)",
                V{id, std::string("Launch review"), std::string("ended"), std::string(kDate),
                  std::string(kDate), 0.0, std::string(kPrivate), std::string(kPrivate),
                  std::int64_t{0}, std::string(kPrivate),
                  id == "m_empty" ? null : sql::Value(state), std::int64_t{7},
                  std::string(R"({"url": "private-marker"})"), text_or_null(origin)});
        }
        for (const auto& [id, meeting, start] :
             std::vector<std::tuple<std::string, std::string, double>>{
                 {"sg_a", "m_a", 0.0},
                 {"sg_b", "m_a", 0.0},
                 {"sg_c", "m_a", 10.0},
                 {"sg_remote", "m_remote", 0.0}}) {
            const std::string blob(kPrivate);
            sql::Blob embedding;
            for (const char c : blob) {
                embedding.push_back(static_cast<std::byte>(c));
            }
            run(connection,
                "INSERT INTO meeting_segments (id, meeting_id, channel, start_s, end_s, text, "
                "speaker_participant_id, speaker_source, speaker_pinned, embedding, created_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                V{id, meeting, std::string("mic"), start, start + 3,
                  std::string("We launch Friday"), std::string("p_a"), std::string("human"),
                  std::int64_t{0}, embedding, std::string(kDate)});
        }
        transaction.commit();
    });
    database.close();
    return path;
}

void add_history_entry(const std::filesystem::path& database, std::string_view id,
                       std::string_view text, std::string_view timestamp, std::string_view model) {
    Database db = Database::open_or_create(database);
    db.write([&](sql::Connection& connection) {
        run(connection,
            "INSERT INTO transcription_history (id, text, timestamp, model) VALUES (?, ?, ?, ?)",
            {std::string(id), std::string(text), std::string(timestamp), std::string(model)});
    });
    db.close();
}

void write_sql(const std::filesystem::path& database, std::string_view sql) {
    Database db = Database::open_or_create(database);
    db.write([&](sql::Connection& connection) { connection.exec(sql); });
    db.close();
}

} // namespace fv::agentapi::test
