#pragma once

// An MCP server over loopback HTTP, driving the live session.
//
// The point of embedding it rather than shipping a separate headless tool is
// that the agent and the person are looking at the same thing: the process this
// attaches to is the one on screen, the scan it starts fills the Scanner panel,
// and an address it adds appears in the address list a frame later. A separate
// process would have its own RuntimeServices and none of that would be true.
//
// It is off until someone starts it. That is deliberate and it is the whole of
// the consent model: the tools include memory writes, code patches and remote
// thread creation, none of which ask again once the server is running. Starting
// it is the decision; see docs/mcp-api.md, which says so in the same words.

#include "infra/Result.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpTools.h"
#include "services/RuntimeServices.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ire::mcp {

class McpServer {
public:
    explicit McpServer(services::RuntimeServices& services);
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Binds 127.0.0.1 on `port` and starts serving. Port 0 lets the OS choose,
    // and port() then reports what it chose. Fails with a sentence rather than a
    // code -- "that port is already in use" is something the user can act on.
    infra::Result<void> start(std::uint16_t port);
    void stop();

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::string token() const;
    // The URL a client is configured with, without the token.
    [[nodiscard]] std::string url() const;

    // Log lines produced so far, removed from the buffer as they are taken --
    // the same arrangement as LuaConsole::takeOutput, so the panel owns what it
    // has already shown and the server's buffer cannot grow without bound.
    [[nodiscard]] std::vector<std::string> takeLog();
    [[nodiscard]] std::uint64_t requestCount() const { return requests_; }

    [[nodiscard]] const ToolRegistry& tools() const { return tools_; }

private:
    void serve();
    // One connection, start to finish. Runs on the accept thread, which is what
    // serialises tool calls: a second client waits rather than interleaving.
    void handleConnection(std::uintptr_t client);
    void append(std::string line);

    services::RuntimeServices& services_;
    ToolRegistry tools_;
    Protocol protocol_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requests_{0};
    std::uint16_t port_{};
    // INVALID_SOCKET, spelled without including winsock2.h in this header.
    std::uintptr_t listener_{~std::uintptr_t{0}};
    bool winsockReady_{};
    std::jthread worker_;

    mutable std::mutex mutex_;
    std::string token_;
    std::vector<std::string> log_;
};

} // namespace ire::mcp
