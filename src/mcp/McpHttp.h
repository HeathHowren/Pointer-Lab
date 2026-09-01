#pragma once

// Just enough HTTP/1.1 to carry JSON-RPC over loopback.
//
// Written by hand rather than pulled in, because what MCP's Streamable HTTP
// transport actually needs from a server is one POST with a Content-Length and
// one JSON reply. A general HTTP library would be several times the size of
// everything else added here and would bring a second parser for a protocol
// this speaks four verbs of.
//
// The parsing is kept free of sockets so it can be tested without binding a
// port: a test suite that opens a real listener on a contributor's machine is
// one that fails in CI for reasons unrelated to the change.

#include <cstddef>
#include <map>
#include <string>

namespace ire::mcp {

struct HttpRequest {
    std::string method;
    std::string target;
    // Keys lowercased, because HTTP header names are case-insensitive and a
    // client that sends `content-length` is as correct as one that sends
    // `Content-Length`.
    std::map<std::string, std::string> headers;
    std::string body;

    [[nodiscard]] std::string header(const std::string& name) const;
};

struct HttpParse {
    enum class State {
        // The bytes so far are a valid prefix; read more and try again.
        Incomplete,
        Complete,
        Malformed
    };

    State state{State::Incomplete};
    HttpRequest request;
    std::string error;
    // How many bytes of the buffer this request consumed, so a connection that
    // pipelined a second request does not lose it.
    std::size_t consumed{};
};

[[nodiscard]] HttpParse parseHttpRequest(const std::string& buffer);

// A complete response, headers and all. Connection: close because each exchange
// is independent and keeping the socket alive would mean tracking idle ones.
[[nodiscard]] std::string httpResponse(int status, const std::string& body,
                                       const std::string& contentType = "application/json");

// Whether `authorization` carries exactly this bearer token.
//
// The comparison does not stop at the first wrong byte. On loopback that is
// close to theatre, but the alternative is a comparison whose duration reports
// how much of the token was right, and writing that deliberately is not a habit
// worth having.
[[nodiscard]] bool bearerTokenMatches(const std::string& authorization, const std::string& token);

// A token with 128 bits of entropy from the OS's own generator, hex-encoded.
[[nodiscard]] std::string generateToken();

} // namespace ire::mcp
