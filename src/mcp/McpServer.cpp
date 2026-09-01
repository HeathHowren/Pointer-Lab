#include "mcp/McpServer.h"

#include "infra/Logger.h"
#include "mcp/McpHttp.h"
#include "platform_win32/Win32Platform.h"

// Before Windows.h, which pulls in the original winsock.h and leaves the two
// fighting over the same symbols. WIN32_LEAN_AND_MEAN is defined project-wide so
// in practice it does not, but the ordering is the documented contract and the
// failure when it is wrong is a wall of redefinition errors.
#include <winsock2.h>

#include <ws2tcpip.h>

#include <chrono>

namespace ire::mcp {

namespace {

template <typename T>
using Result = infra::Result<T>;

constexpr std::uintptr_t invalidSocket = ~std::uintptr_t{0};

// Long enough that a client pausing between its headers and its body is not cut
// off, short enough that a connection which opened and then said nothing does
// not hold the accept loop. The loop is single-threaded on purpose -- that is
// what serialises tool calls -- so this timeout is the only thing preventing one
// silent client from being a denial of service against the person at the window.
constexpr int receiveTimeoutMs = 15000;

// The log the panel shows. Bounded because a session left running overnight
// against a chatty agent would otherwise hold every line it ever wrote.
constexpr std::size_t maxLogLines = 500;

} // namespace

McpServer::McpServer(services::RuntimeServices& services)
    : services_(services), tools_(services), protocol_(tools_) {
    protocol_.setLogger([this](std::string line) { append(std::move(line)); });
}

McpServer::~McpServer() {
    stop();
}

std::string McpServer::token() const {
    std::scoped_lock lock(mutex_);
    return token_;
}

std::string McpServer::url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

void McpServer::append(std::string line) {
    std::scoped_lock lock(mutex_);
    if (log_.size() >= maxLogLines) {
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - maxLogLines + 1));
    }
    log_.push_back(std::move(line));
}

std::vector<std::string> McpServer::takeLog() {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> taken;
    taken.swap(log_);
    return taken;
}

infra::Result<void> McpServer::start(std::uint16_t port) {
    if (running_) {
        return Result<void>::fail("The MCP server is already running.");
    }

    if (!winsockReady_) {
        WSADATA data{};
        const int started = WSAStartup(MAKEWORD(2, 2), &data);
        if (started != 0) {
            return Result<void>::fail("Windows Sockets could not be started.",
                                      static_cast<infra::ErrorCode>(started));
        }
        winsockReady_ = true;
    }

    auto newToken = generateToken();
    if (newToken.empty()) {
        return Result<void>::fail("A session token could not be generated, so the server was not started.");
    }

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return Result<void>::fail("A listening socket could not be created.",
                                  static_cast<infra::ErrorCode>(WSAGetLastError()));
    }

    // Deliberately no SO_REUSEADDR. On Windows it lets a second process bind a
    // port another one is already listening on, which for a socket that grants
    // memory-write access to this process is precisely the wrong trade: a
    // failed bind that says "that port is in use" is the correct outcome.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    // Loopback rather than INADDR_ANY. Nothing here should ever be reachable
    // from another machine, and binding the wildcard and relying on a firewall
    // to make up the difference is not a decision this should be making for
    // somebody.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const auto error = static_cast<infra::ErrorCode>(WSAGetLastError());
        closesocket(listener);
        if (error == WSAEADDRINUSE) {
            return Result<void>::fail("Port " + std::to_string(port) +
                                          " is already in use. Choose another, or use 0 to let Windows pick.",
                                      error);
        }
        return Result<void>::fail("Port " + std::to_string(port) + " could not be bound: " +
                                      platform_win32::Win32Platform::formatLastError(error),
                                  error);
    }

    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        const auto error = static_cast<infra::ErrorCode>(WSAGetLastError());
        closesocket(listener);
        return Result<void>::fail("The socket could not be listened on.", error);
    }

    // Port 0 means the OS chose one, and the caller has no other way to find out
    // which. Asked for in every case so port() is always the truth rather than
    // the request.
    sockaddr_in bound{};
    int boundSize = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    {
        std::scoped_lock lock(mutex_);
        token_ = std::move(newToken);
    }
    listener_ = static_cast<std::uintptr_t>(listener);
    running_ = true;
    worker_ = std::jthread([this] { serve(); });

    infra::Logger::instance().info("MCP server listening on " + url() + ".");
    append("Listening on " + url() + ".");
    return Result<void>::ok();
}

void McpServer::stop() {
    if (!running_ && listener_ == invalidSocket) {
        return;
    }
    running_ = false;

    // Closing the listener is what breaks accept(): there is no portable way to
    // interrupt a blocking accept, and a self-connect to wake it would need the
    // very port that is being torn down.
    if (listener_ != invalidSocket) {
        closesocket(static_cast<SOCKET>(listener_));
        listener_ = invalidSocket;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::scoped_lock lock(mutex_);
        token_.clear();
    }
    if (winsockReady_) {
        WSACleanup();
        winsockReady_ = false;
    }
    infra::Logger::instance().info("MCP server stopped.");
}

void McpServer::serve() {
    while (running_) {
        const SOCKET client = accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            // Either the listener was closed under us by stop(), which is the
            // ordinary way out, or the accept failed for its own reasons. Both
            // end the loop; a tight retry on a broken listener would spin.
            break;
        }
        handleConnection(static_cast<std::uintptr_t>(client));
    }
}

void McpServer::handleConnection(std::uintptr_t clientHandle) {
    const SOCKET client = static_cast<SOCKET>(clientHandle);

    DWORD timeout = receiveTimeoutMs;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    const auto reply = [client](const std::string& text) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const int wrote = send(client, text.data() + sent, static_cast<int>(text.size() - sent), 0);
            if (wrote <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(wrote);
        }
    };

    std::string buffer;
    HttpParse parse;
    char chunk[8192];
    while (true) {
        parse = parseHttpRequest(buffer);
        if (parse.state != HttpParse::State::Incomplete) {
            break;
        }
        const int received = recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (received <= 0) {
            closesocket(client);
            return;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
    }

    if (parse.state == HttpParse::State::Malformed) {
        reply(httpResponse(400, Json{{"error", parse.error}}.dump()));
        closesocket(client);
        return;
    }

    const auto& request = parse.request;

    // Authorisation before anything else, including the method check, so a
    // caller without the token learns nothing about what the server would have
    // accepted.
    if (!bearerTokenMatches(request.header("authorization"), token())) {
        append("Rejected a request with no valid token.");
        reply(httpResponse(401,
                           Json{{"error", "A valid bearer token is required."}}.dump()));
        closesocket(client);
        return;
    }

    if (request.method == "OPTIONS") {
        reply(httpResponse(405, Json{{"error", "This server does not accept cross-origin requests."}}.dump()));
        closesocket(client);
        return;
    }
    if (request.method == "GET") {
        // The GET half of Streamable HTTP is the server-to-client event stream.
        // Nothing here pushes events yet -- every tool is request/response -- so
        // saying so plainly is better than holding a stream open that will never
        // carry anything.
        reply(httpResponse(405, Json{{"error", "This server does not offer an event stream. Use POST."}}.dump()));
        closesocket(client);
        return;
    }
    if (request.method != "POST") {
        reply(httpResponse(405, Json{{"error", "Only POST is supported."}}.dump()));
        closesocket(client);
        return;
    }

    ++requests_;
    const auto response = protocol_.handle(request.body);
    if (response.empty()) {
        // A notification. JSON-RPC says it takes no reply, and MCP's HTTP
        // transport says to answer 202 with no body.
        reply(httpResponse(202, {}, "text/plain"));
    } else {
        reply(httpResponse(200, response));
    }
    closesocket(client);
}

} // namespace ire::mcp
