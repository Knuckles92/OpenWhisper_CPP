#pragma once

#include <functional>
#include <memory>

#include <QString>

#include "agentapi/mcp_runtime.h"

namespace fv {
class AppContext;
class SettingsStore;

// Only a fresh request from the desktop settings page starts the loopback listener.
std::shared_ptr<agentapi::McpServerControl>
sessionMcpControl(std::shared_ptr<agentapi::McpServerControl> listener, SettingsStore& settings,
                  std::function<bool(const QString&)> confirm);
// Applies committed MCP mutations to the desktop on its UI executor.
std::function<void(std::string, Json)> sessionMcpDesktopChanges(AppContext&);
void provideSessionMcp(AppContext&);
} // namespace fv
