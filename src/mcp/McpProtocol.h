#pragma once

// JSON-RPC 2.0 framing and the handful of MCP methods above it.
//
// Deliberately transport-free: it turns one request body into one response body
// and knows nothing about sockets. That is what makes it testable without
// opening a port, which matters more here than usual -- the alternative is a
// test suite that binds to a real port on a contributor's machine.

#include "mcp/McpTools.h"

#include <functional>
#include <string>

namespace ire::mcp {

// The spec revision this speaks. A client asking for a different one is answered
// with this rather than refused: the methods used here have not changed across
// revisions, and refusing a client over a date string helps nobody.
inline constexpr const char* protocolVersion = "2025-06-18";

class Protocol {
public:
    explicit Protocol(ToolRegistry& tools) : tools_(tools) {}

    // Handles one request body. Returns the response body, or an empty string
    // for a notification -- JSON-RPC says a message with no id gets no reply,
    // and `notifications/initialized` is one every client sends.
    [[nodiscard]] std::string handle(const std::string& body);

    // Called with a one-line summary of every tool call. The server uses this
    // for the panel's request log; nothing here depends on it being set.
    void setLogger(std::function<void(std::string)> logger) { logger_ = std::move(logger); }

private:
    // Returns the response, or a null Json for a notification.
    [[nodiscard]] Json dispatch(const Json& request);
    [[nodiscard]] Json callTool(const Json& params);

    ToolRegistry& tools_;
    std::function<void(std::string)> logger_;
};

} // namespace ire::mcp
