#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// The in-app MCP server's 22 tools (services/agent_mcp/app.py): history retrieval through the
// History API in process, the user-granted controls, the live tools over the app's
// automation::AppControl and event bus, and the argument validation the MCP SDK's pydantic models
// give them. Tool failures reach the agent as text, never as exceptions.
namespace fv {
class SettingsStore;
}

namespace fv::agentapi {

class AgentControls;
class HistoryApi;

// Where the MCP endpoint is served.
inline constexpr std::string_view MCP_PATH = "/mcp";
// Longest text run_action accepts, as the live API's POST /v1/actions/{id}/run (code points).
inline constexpr std::size_t MAX_ACTION_TEXT = 20'000;
// Longest notice text and title, as the live API's POST /v1/notify (code points).
inline constexpr std::size_t MAX_NOTICE_TEXT = 500;
inline constexpr std::size_t MAX_NOTICE_TITLE = 100;
// A record or meeting id: 1 to 200 code points, none of / \ ? # or a control character.
inline constexpr std::string_view RECORD_ID_PATTERN = R"(^[^/\\?#\x00-\x1f]+$)";

// data/mcp_tools.json (tools/gen/mcp_tools.py) with identity().product in its product names:
// the server's name and version, its instructions, and the tools as the 2025-11-25 wire lists
// them, in registration order.
struct McpCatalog {
    Json server; // {"name", "version"}
    std::string instructions;
    Json tools; // [{"annotations", "description", "inputSchema", "name", "outputSchema"}]
};
const McpCatalog& mcp_catalog();

// _tool_annotations: the hints of one kind of tool (read, write, run, record, stop, cancel,
// notice, live_read), keys in wire order; throws std::out_of_range for another kind.
Json tool_annotations(std::string_view kind);

// What a tools/call answers.
struct ToolResult {
    bool is_error = false;
    Json structured;  // the tool's object; null for an error
    std::string text; // the object as indent-2 JSON, or the error text the agent reads
};

struct McpToolsOptions {
    HistoryApi* history = nullptr; // answers the history tools in process
    std::string token;             // the History API token those requests carry
    AgentControls* controls = nullptr;
    std::function<bool()> enabled;     // null: always on
    SettingsStore* settings = nullptr; // live permissions, re-read on every call; null: none saved
    bool local_history_only = false;
    bool allow_live_tools = true;
};

// Affinity: thread-safe; blocking (SQLite, the settings file, AppControl's two seconds), never
// on the GUI thread or the reactor.
class McpTools {
public:
    explicit McpTools(McpToolsOptions options);
    ~McpTools();
    McpTools(const McpTools&) = delete;
    McpTools& operator=(const McpTools&) = delete;

    bool has(std::string_view name) const;
    Json listed(const Json& catalog) const;
    std::string instructions() const;
    // Validates `arguments` (an object) as the tool's pydantic model does, then runs the tool.
    // A validation failure, a refusal or a failure is an error result whose text starts
    // "Error executing tool <name>"; nullopt for a name no tool has.
    std::optional<ToolResult> call(std::string_view name, const Json& arguments);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fv::agentapi
