#pragma once

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentapi/api_errors.h"
#include "agentapi/history_routes.h"
#include "base/json.h"
#include "net/http_server.h"

// Shared by the tests of fv_agentapi.
namespace fv::agentapi::test {

// tests/test_agent_api.py TOKEN.
inline const std::string kToken = "test-history-token-" + std::string(32, 'a');
// tests/test_agent_api.py DATE.
inline constexpr std::string_view kDate = "2026-09-01T12:00:00+00:00";

using Pairs = std::vector<std::pair<std::string, std::string>>;

// A request as TestClient(base_url="http://127.0.0.1", headers={"Authorization": "Bearer
// <TOKEN>"}) sends it. `headers` replace those two by name; a header given an empty optional is
// left out.
net::HttpRequest
request(std::string path, Pairs query = {},
        std::vector<std::pair<std::string, std::optional<std::string>>> headers = {},
        std::string method = "GET");

// handle() and the body parsed as JSON (null when it is not JSON).
struct Reply {
    ApiResponse response;
    Json json;
    int status() const { return response.status; }
    std::optional<std::string> header(std::string_view name) const;
};
Reply get(HistoryApi&, std::string path, Pairs query = {},
          std::vector<std::pair<std::string, std::optional<std::string>>> headers = {});

// all_pages: every item of a paged route, following next_cursor; fails the test on a non-200
// answer or a repeated cursor.
std::vector<Json> all_pages(HistoryApi&, const std::string& path, Pairs query);
std::vector<std::string> ids(const std::vector<Json>& items);

// tests/fixtures/agentapi/history.db, made by OpenWhisper (make_history_fixture.py), copied into
// `folder` under `name`.
std::filesystem::path copy_history_fixture(const std::filesystem::path& folder,
                                           std::string_view name = "history.db");

// The `database` fixture of tests/test_agent_api.py, made with fv::Database in `folder` as
// "history # café.db": five dictations (one a remote copy), three meetings (one remote, one
// without a snapshot) and four segments.
std::filesystem::path make_test_database(const std::filesystem::path& folder);

// DatabaseManager.add_history_entry(entry_id, text, timestamp, model).
void add_history_entry(const std::filesystem::path& database, std::string_view id,
                       std::string_view text, std::string_view timestamp, std::string_view model);
// Runs `sql` on a writable connection of `database` (the app's own writes).
void write_sql(const std::filesystem::path& database, std::string_view sql);

} // namespace fv::agentapi::test
