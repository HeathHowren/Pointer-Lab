// MCP server tests.
//
// Almost everything here runs without opening a socket and without attaching to
// anything. That is deliberate: the transport is split from the parsing so the
// protocol can be tested without a listener, and the tools are thin enough --
// each calls an engine that has its own tests -- that what is worth checking is
// the layer between JSON and those engines: framing, dispatch, argument
// validation, and the mapping from infra::Result onto a tool result.
//
// The exception is the group at the end, which binds a real listener and speaks
// real HTTP to it. Without one of those, nothing proves the pieces are wired
// together at all -- every test above would still pass if the server never
// listened. It asks for port 0 so it cannot collide with anything, and it skips
// rather than fails where the environment refuses a socket.

#include <catch2/catch_test_macros.hpp>

#include "mcp/McpHttp.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpServer.h"
#include "mcp/McpTools.h"
#include "services/RuntimeServices.h"

#include <winsock2.h>

#include <ws2tcpip.h>

#include <string>

using namespace ire;
using ire::mcp::Json;

namespace {

// A registry needs a full service stack, which owns its own session. Nothing is
// attached, so every tool that touches a target reports that rather than
// crashing -- which is itself one of the things worth testing.
struct Server {
    services::RuntimeServices services;
    mcp::ToolRegistry tools{services};
    mcp::Protocol protocol{tools};
};

Json request(mcp::Protocol& protocol, const std::string& method, Json params = Json::object(), int id = 1) {
    const Json message{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    const auto response = protocol.handle(message.dump());
    REQUIRE_FALSE(response.empty());
    return Json::parse(response);
}

// The text of a tools/call result, which is where a tool's answer and its error
// message both end up.
std::string callText(const Json& response) {
    REQUIRE(response.contains("result"));
    const auto& result = response.at("result");
    REQUIRE(result.contains("content"));
    REQUIRE(result.at("content").is_array());
    REQUIRE_FALSE(result.at("content").empty());
    return result.at("content").at(0).at("text").get<std::string>();
}

bool callFailed(const Json& response) {
    return response.at("result").at("isError").get<bool>();
}

Json callTool(mcp::Protocol& protocol, const std::string& name, Json arguments = Json::object()) {
    return request(protocol, "tools/call", Json{{"name", name}, {"arguments", std::move(arguments)}});
}

} // namespace

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

TEST_CASE("A complete POST parses into method, headers and body", "[mcp][http]") {
    const std::string raw =
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1:8722\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"hello\":123}";

    const auto parsed = mcp::parseHttpRequest(raw);
    REQUIRE(parsed.state == mcp::HttpParse::State::Complete);
    CHECK(parsed.request.method == "POST");
    CHECK(parsed.request.target == "/mcp");
    CHECK(parsed.request.body == "{\"hello\":123}");
    CHECK(parsed.consumed == raw.size());
}

TEST_CASE("Header names are matched without regard to case", "[mcp][http]") {
    const std::string raw =
        "POST / HTTP/1.1\r\n"
        "CoNtEnT-lEnGtH: 2\r\n"
        "AUTHORIZATION: Bearer abc\r\n"
        "\r\n"
        "{}";

    const auto parsed = mcp::parseHttpRequest(raw);
    REQUIRE(parsed.state == mcp::HttpParse::State::Complete);
    // HTTP says header names are case-insensitive, and clients differ on which
    // casing they send. Getting this wrong would reject every request from one
    // client while accepting every request from another.
    CHECK(parsed.request.header("content-length") == "2");
    CHECK(parsed.request.header("Authorization") == "Bearer abc");
}

TEST_CASE("A request whose body has not arrived is incomplete rather than malformed", "[mcp][http]") {
    // The distinction is the whole reason the parser is called in a loop: a
    // truncated read must ask for more bytes, not answer 400.
    const std::string raw =
        "POST / HTTP/1.1\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "{\"partial\":";

    const auto parsed = mcp::parseHttpRequest(raw);
    CHECK(parsed.state == mcp::HttpParse::State::Incomplete);
}

TEST_CASE("Headers alone with no body terminator are incomplete", "[mcp][http]") {
    const auto parsed = mcp::parseHttpRequest("POST / HTTP/1.1\r\nContent-Length: 2\r\n");
    CHECK(parsed.state == mcp::HttpParse::State::Incomplete);
}

TEST_CASE("A non-numeric Content-Length is rejected", "[mcp][http]") {
    const auto parsed = mcp::parseHttpRequest("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
    CHECK(parsed.state == mcp::HttpParse::State::Malformed);
}

TEST_CASE("A request with no Content-Length parses with an empty body", "[mcp][http]") {
    const auto parsed = mcp::parseHttpRequest("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(parsed.state == mcp::HttpParse::State::Complete);
    CHECK(parsed.request.method == "GET");
    CHECK(parsed.request.body.empty());
}

TEST_CASE("A response carries the body's length", "[mcp][http]") {
    const auto response = mcp::httpResponse(200, "{\"a\":1}");
    CHECK(response.find("HTTP/1.1 200 OK") == 0);
    CHECK(response.find("Content-Length: 7") != std::string::npos);
    CHECK(response.find("\r\n\r\n{\"a\":1}") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The token
// ---------------------------------------------------------------------------

TEST_CASE("A bearer token is accepted only when it matches exactly", "[mcp][token]") {
    const std::string token = "0123456789abcdef0123456789abcdef";

    CHECK(mcp::bearerTokenMatches("Bearer " + token, token));
    // The scheme is case-insensitive per RFC 7235, and clients differ.
    CHECK(mcp::bearerTokenMatches("bearer " + token, token));

    CHECK_FALSE(mcp::bearerTokenMatches("Bearer " + token + "0", token));
    CHECK_FALSE(mcp::bearerTokenMatches("Bearer 0123456789abcdef0123456789abcde0", token));
    CHECK_FALSE(mcp::bearerTokenMatches(token, token));
    CHECK_FALSE(mcp::bearerTokenMatches("", token));
    CHECK_FALSE(mcp::bearerTokenMatches("Basic " + token, token));
}

TEST_CASE("An empty server token accepts nothing", "[mcp][token]") {
    // A stopped server clears its token. If an empty token matched an empty
    // header, stopping the server would open it to everything.
    CHECK_FALSE(mcp::bearerTokenMatches("Bearer ", ""));
    CHECK_FALSE(mcp::bearerTokenMatches("", ""));
}

TEST_CASE("Generated tokens are long and do not repeat", "[mcp][token]") {
    const auto first = mcp::generateToken();
    const auto second = mcp::generateToken();
    CHECK(first.size() == 32);
    CHECK(second.size() == 32);
    CHECK(first != second);
}

// ---------------------------------------------------------------------------
// JSON-RPC framing
// ---------------------------------------------------------------------------

TEST_CASE("Malformed JSON is answered with a parse error", "[mcp][protocol]") {
    Server server;
    const auto response = Json::parse(server.protocol.handle("{not json"));
    REQUIRE(response.contains("error"));
    CHECK(response.at("error").at("code").get<int>() == -32700);
}

TEST_CASE("An unknown method is answered with method not found", "[mcp][protocol]") {
    Server server;
    const auto response = request(server.protocol, "no/such/method");
    REQUIRE(response.contains("error"));
    CHECK(response.at("error").at("code").get<int>() == -32601);
}

TEST_CASE("A notification is not answered at all", "[mcp][protocol]") {
    Server server;
    // Every client sends this immediately after initialize. Answering it with a
    // method-not-found is the single most common way to make one hang up, so it
    // has to produce no reply rather than an error reply.
    const Json message{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    CHECK(server.protocol.handle(message.dump()).empty());

    const Json unknown{{"jsonrpc", "2.0"}, {"method", "no/such/method"}};
    CHECK(server.protocol.handle(unknown.dump()).empty());
}

TEST_CASE("initialize reports the server and echoes a known protocol version", "[mcp][protocol]") {
    Server server;
    const auto response = request(server.protocol, "initialize",
                                  Json{{"protocolVersion", "2025-03-26"}});
    const auto& result = response.at("result");
    CHECK(result.at("serverInfo").at("name") == "pointerlab");
    CHECK(result.at("protocolVersion") == "2025-03-26");
    CHECK(result.at("capabilities").contains("tools"));
}

TEST_CASE("A batch of requests is answered with a batch of responses", "[mcp][protocol]") {
    Server server;
    const Json batch = Json::array({Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
                                    Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}});
    const auto response = Json::parse(server.protocol.handle(batch.dump()));
    REQUIRE(response.is_array());
    CHECK(response.size() == 2);
}

// ---------------------------------------------------------------------------
// The tool surface
// ---------------------------------------------------------------------------

TEST_CASE("Every tool has a name, a description and an object schema", "[mcp][tools]") {
    Server server;
    const auto response = request(server.protocol, "tools/list");
    const auto& tools = response.at("result").at("tools");
    REQUIRE(tools.is_array());
    CHECK(tools.size() > 40);

    for (const auto& tool : tools) {
        const auto name = tool.at("name").get<std::string>();
        INFO("tool: " << name);
        CHECK_FALSE(name.empty());
        // The description is the only documentation the model gets. An empty one
        // is a tool nothing will call correctly.
        CHECK(tool.at("description").get<std::string>().size() > 20);
        CHECK(tool.at("inputSchema").at("type") == "object");
        CHECK(tool.at("inputSchema").contains("properties"));
    }
}

TEST_CASE("Tool names are unique", "[mcp][tools]") {
    Server server;
    std::vector<std::string> names;
    for (const auto& tool : server.tools.tools()) {
        names.push_back(tool.name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST_CASE("The tools that mirror a Lua function keep its name", "[mcp][tools]") {
    // The Lua function names are a published contract for the life of 3.x. Where
    // a tool does the same job it takes the same name, so there is one
    // vocabulary to learn rather than two.
    Server server;
    for (const char* name : {"processes", "attach", "detach", "modules", "regions", "read", "write",
                             "read_bytes", "write_bytes", "resolve", "add_address", "alloc",
                             "load_library", "screenshot", "select_panel", "set_layout",
                             "set_window_size", "quit"}) {
        INFO("expected tool: " << name);
        CHECK(server.tools.find(name) != nullptr);
    }
}

TEST_CASE("An unknown tool is a tool error rather than a protocol error", "[mcp][tools]") {
    Server server;
    // The distinction matters: a protocol error is the client's bug and is not
    // shown to the model, while this is something the model is meant to read and
    // correct itself from.
    const auto response = callTool(server.protocol, "no_such_tool");
    CHECK(callFailed(response));
    CHECK(callText(response).find("no_such_tool") != std::string::npos);
}

TEST_CASE("A tool needing a target says so when nothing is attached", "[mcp][tools]") {
    Server server;
    for (const char* name : {"read", "read_bytes", "modules", "regions", "disassemble"}) {
        INFO("tool: " << name);
        const auto response = callTool(server.protocol, name, Json{{"address", 0x1000}, {"size", 4}});
        CHECK(callFailed(response));
        CHECK(callText(response).find("attach") != std::string::npos);
    }
}

TEST_CASE("processes works with nothing attached", "[mcp][tools]") {
    Server server;
    // It enumerates the machine rather than a target, so it is one of the few
    // tools that must answer before anything is attached -- it is how a caller
    // finds the pid to attach to.
    const auto response = callTool(server.protocol, "processes");
    REQUIRE_FALSE(callFailed(response));
    const auto parsed = Json::parse(callText(response));
    REQUIRE(parsed.at("processes").is_array());
    CHECK_FALSE(parsed.at("processes").empty());
}

TEST_CASE("session_info reports nothing attached rather than failing", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "session_info");
    REQUIRE_FALSE(callFailed(response));
    CHECK(Json::parse(callText(response)).at("attached").get<bool>() == false);
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST_CASE("A missing required argument is named in the error", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "attach");
    CHECK(callFailed(response));
    // "pid is required" is something a model can act on; "bad argument" is not.
    CHECK(callText(response).find("pid") != std::string::npos);
}

TEST_CASE("An argument of the wrong type is named in the error", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "attach", Json{{"pid", "not a number"}});
    CHECK(callFailed(response));
    CHECK(callText(response).find("pid") != std::string::npos);
}

TEST_CASE("An unrecognised value type lists the ones that exist", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "read",
                                   Json{{"address", 0x1000}, {"type", "quadruple"}});
    CHECK(callFailed(response));
    const auto text = callText(response);
    CHECK(text.find("quadruple") != std::string::npos);
    CHECK(text.find("i32") != std::string::npos);
}

TEST_CASE("Scan modes are accepted by their display names, however spaced", "[mcp][tools]") {
    Server server;
    // The vocabulary the model is given is the vocabulary the window shows, so
    // a person reading the agent's transcript recognises what it did.
    for (const char* mode : {"Exact value", "exact value", "increased by", "IncreasedBy",
                             "same_as_first_scan"}) {
        INFO("mode: " << mode);
        const auto response = callTool(server.protocol, "scan_first",
                                       Json{{"mode", mode}, {"value", 1}});
        // Nothing is attached, so this fails -- but on the target, not the mode.
        CHECK(callText(response).find("not a scan mode") == std::string::npos);
    }

    const auto bad = callTool(server.protocol, "scan_first", Json{{"mode", "sideways"}});
    CHECK(callFailed(bad));
    CHECK(callText(bad).find("not a scan mode") != std::string::npos);
}

TEST_CASE("scan_status and scan_results answer before any scan has run", "[mcp][tools]") {
    Server server;
    const auto status = callTool(server.protocol, "scan_status");
    REQUIRE_FALSE(callFailed(status));
    CHECK(Json::parse(callText(status)).at("running").get<bool>() == false);

    const auto results = callTool(server.protocol, "scan_results");
    REQUIRE_FALSE(callFailed(results));
    const auto parsed = Json::parse(callText(results));
    CHECK(parsed.at("total").get<std::size_t>() == 0);
    CHECK(parsed.at("results").is_array());
}

TEST_CASE("scan_next refuses when there is nothing to narrow", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "scan_next", Json{{"mode", "changed"}});
    CHECK(callFailed(response));
}

// ---------------------------------------------------------------------------
// The address list, which works without a target
// ---------------------------------------------------------------------------

TEST_CASE("An address list entry can be added, listed and removed", "[mcp][tools]") {
    Server server;

    const auto added = callTool(server.protocol, "add_address",
                                Json{{"address", 0x140001000},
                                     {"type", "u32"},
                                     {"description", "test entry"},
                                     {"group", "tests"}});
    REQUIRE_FALSE(callFailed(added));
    const auto id = Json::parse(callText(added)).at("id").get<std::uint64_t>();

    const auto listed = callTool(server.protocol, "list_addresses");
    REQUIRE_FALSE(callFailed(listed));
    const auto entries = Json::parse(callText(listed)).at("entries");
    REQUIRE(entries.size() == 1);
    CHECK(entries.at(0).at("id").get<std::uint64_t>() == id);
    CHECK(entries.at(0).at("description") == "test entry");
    CHECK(entries.at(0).at("type") == "u32");
    // No target, so there is no value to report -- but the entry is still real.
    CHECK_FALSE(entries.at(0).contains("value"));

    const auto removed = callTool(server.protocol, "remove_address", Json{{"id", id}});
    CHECK_FALSE(callFailed(removed));

    const auto again = callTool(server.protocol, "list_addresses");
    CHECK(Json::parse(callText(again)).at("entries").empty());
}

TEST_CASE("Removing an entry that is not there says so", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "remove_address", Json{{"id", 9999}});
    CHECK(callFailed(response));
    CHECK(callText(response).find("9999") != std::string::npos);
}

TEST_CASE("A chain-backed entry records its chain", "[mcp][tools]") {
    Server server;
    const auto added = callTool(server.protocol, "add_chain_address",
                                Json{{"module", "game.exe"},
                                     {"module_offset", 0x3040},
                                     {"offsets", Json::array({0x10, 0x8})},
                                     {"type", "f32"}});
    REQUIRE_FALSE(callFailed(added));

    const auto listed = Json::parse(callText(callTool(server.protocol, "list_addresses")));
    const auto& entry = listed.at("entries").at(0);
    REQUIRE(entry.contains("chain"));
    CHECK(entry.at("chain").at("module") == "game.exe");
    CHECK(entry.at("chain").at("offsets").size() == 2);
}

TEST_CASE("A chain with no module and no offset is refused", "[mcp][tools]") {
    Server server;
    // Such a chain has nothing to start from and would resolve to whatever
    // happened to be at zero.
    const auto response = callTool(server.protocol, "add_chain_address", Json{{"module_offset", 0}});
    CHECK(callFailed(response));
}

TEST_CASE("Chain offsets must be integers", "[mcp][tools]") {
    Server server;
    // The Lua API reads a non-numeric offset as 0, and the chain then resolves
    // to the wrong address with nothing to say why. Here it is an error.
    const auto response = callTool(server.protocol, "resolve_chain",
                                   Json{{"module", "game.exe"},
                                        {"module_offset", 0x1000},
                                        {"offsets", Json::array({0x10, "oops"})}});
    CHECK(callFailed(response));
    CHECK(callText(response).find("integers") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Symbols and structures, which also work without a target
// ---------------------------------------------------------------------------

TEST_CASE("A symbol can be defined for a fixed address and listed", "[mcp][tools]") {
    Server server;
    const auto defined = callTool(server.protocol, "symbol_define",
                                  Json{{"name", "playerBase"}, {"expression", "0x140001000"}});
    REQUIRE_FALSE(callFailed(defined));
    CHECK(Json::parse(callText(defined)).at("address").get<std::uint64_t>() == 0x140001000);

    const auto listed = Json::parse(callText(callTool(server.protocol, "symbols_list")));
    REQUIRE(listed.at("symbols").size() == 1);
    CHECK(listed.at("symbols").at(0).at("name") == "playerBase");

    // And an address argument anywhere then accepts that name.
    const auto resolved = callTool(server.protocol, "resolve", Json{{"expression", "playerBase+0x10"}});
    REQUIRE_FALSE(callFailed(resolved));
    CHECK(Json::parse(callText(resolved)).at("address").get<std::uint64_t>() == 0x140001010);
}

TEST_CASE("An expression that names nothing fails with a reason", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "resolve", Json{{"expression", "nosuchmodule.dll+0x10"}});
    CHECK(callFailed(response));
    CHECK_FALSE(callText(response).empty());
}

TEST_CASE("Structure fields round-trip through the registry", "[mcp][tools]") {
    Server server;
    const auto created = callTool(server.protocol, "struct_add", Json{{"name", "Player"}});
    REQUIRE_FALSE(callFailed(created));
    const auto id = Json::parse(callText(created)).at("id").get<std::uint64_t>();

    const auto set = callTool(server.protocol, "struct_set_fields",
                              Json{{"id", id},
                                   {"fields", Json::array({Json{{"offset", 0}, {"type", "i32"}, {"name", "health"}},
                                                           Json{{"offset", 8}, {"type", "f32"}, {"name", "x"}}})}});
    REQUIRE_FALSE(callFailed(set));

    const auto listed = Json::parse(callText(callTool(server.protocol, "struct_list")));
    REQUIRE(listed.at("structures").size() == 1);
    const auto& fields = listed.at("structures").at(0).at("fields");
    REQUIRE(fields.size() == 2);
    CHECK(fields.at(0).at("name") == "health");
    CHECK(fields.at(1).at("offset").get<int>() == 8);
}

TEST_CASE("Overlapping structure fields are refused", "[mcp][tools]") {
    Server server;
    const auto id = Json::parse(callText(callTool(server.protocol, "struct_add", Json{{"name", "P"}})))
                        .at("id")
                        .get<std::uint64_t>();
    // An i64 at +0 covers +4, so a field there has to be rejected: the display
    // would have to pick which of the two owns the byte.
    const auto set = callTool(server.protocol, "struct_set_fields",
                              Json{{"id", id},
                                   {"fields", Json::array({Json{{"offset", 0}, {"type", "i64"}},
                                                           Json{{"offset", 4}, {"type", "i32"}}})}});
    CHECK(callFailed(set));
}

TEST_CASE("A variable-width structure field needs a length", "[mcp][tools]") {
    Server server;
    const auto id = Json::parse(callText(callTool(server.protocol, "struct_add", Json{{"name", "P"}})))
                        .at("id")
                        .get<std::uint64_t>();
    const auto set = callTool(server.protocol, "struct_set_fields",
                              Json{{"id", id},
                                   {"fields", Json::array({Json{{"offset", 0}, {"type", "str"}}})}});
    CHECK(callFailed(set));
    CHECK(callText(set).find("length") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The window tools, with no window
// ---------------------------------------------------------------------------

TEST_CASE("Window tools report that there is no window to drive", "[mcp][tools]") {
    Server server;
    // services::uiCommands() is null in a build with no frontend, which is this
    // one. Saying so is better than failing in some way that reads like a bug in
    // the caller's arguments.
    for (const char* name : {"screenshot", "select_panel", "set_layout", "quit", "project_save"}) {
        INFO("tool: " << name);
        const auto response = callTool(server.protocol, name,
                                       Json{{"path", "x.png"}, {"name", "Scanner"}});
        CHECK(callFailed(response));
        CHECK(callText(response).find("no window") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Assembly, which needs no target
// ---------------------------------------------------------------------------

TEST_CASE("assemble turns source into bytes at the given address", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "assemble",
                                   Json{{"source", "nop"}, {"address", 0x140001000}});
    REQUIRE_FALSE(callFailed(response));
    const auto parsed = Json::parse(callText(response));
    CHECK(parsed.at("hex") == "90");
    CHECK(parsed.at("length").get<std::size_t>() == 1);
}

TEST_CASE("assemble reports what the assembler could not encode", "[mcp][tools]") {
    Server server;
    const auto response = callTool(server.protocol, "assemble",
                                   Json{{"source", "not an instruction"}, {"address", 0x1000}});
    CHECK(callFailed(response));
    CHECK_FALSE(callText(response).empty());
}

// ---------------------------------------------------------------------------
// The listener
//
// One group that goes over a real socket, because every test above would still
// pass if the server never listened at all.
// ---------------------------------------------------------------------------

namespace {

// The smallest HTTP client that can ask this server a question. Returns the
// whole response including headers, or an empty string if it could not connect.
std::string sendRaw(std::uint16_t port, const std::string& request) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return {};
    }
    struct Cleanup {
        ~Cleanup() { WSACleanup(); }
    } cleanup;

    const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        return {};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(client);
        return {};
    }

    std::size_t sent = 0;
    while (sent < request.size()) {
        const int wrote = send(client, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (wrote <= 0) {
            closesocket(client);
            return {};
        }
        sent += static_cast<std::size_t>(wrote);
    }

    // The server answers Connection: close, so reading to EOF is the whole
    // response rather than a guess at its length.
    std::string response;
    char chunk[4096];
    while (true) {
        const int received = recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (received <= 0) {
            break;
        }
        response.append(chunk, static_cast<std::size_t>(received));
    }
    closesocket(client);
    return response;
}

std::string post(std::uint16_t port, const std::string& authorization, const std::string& body) {
    std::string request = "POST /mcp HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    if (!authorization.empty()) {
        request += "Authorization: " + authorization + "\r\n";
    }
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    request += body;
    return sendRaw(port, request);
}

// The body of a response, past the header block.
std::string bodyOf(const std::string& response) {
    const auto split = response.find("\r\n\r\n");
    return split == std::string::npos ? std::string() : response.substr(split + 4);
}

} // namespace

TEST_CASE("A tool call arrives over the socket and comes back", "[mcp][server]") {
    services::RuntimeServices services;
    mcp::McpServer server(services);

    if (auto started = server.start(0); !started) {
        SKIP("This environment does not allow a loopback listener: " + started.error());
    }
    REQUIRE(server.running());
    REQUIRE(server.port() != 0);

    const auto authorization = "Bearer " + server.token();
    const Json message{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    const auto response = post(server.port(), authorization, message.dump());

    REQUIRE_FALSE(response.empty());
    CHECK(response.find("HTTP/1.1 200 OK") == 0);
    const auto parsed = Json::parse(bodyOf(response));
    CHECK(parsed.at("id").get<int>() == 1);
    CHECK(parsed.at("result").at("tools").is_array());

    server.stop();
    CHECK_FALSE(server.running());
}

TEST_CASE("A request without the token is refused", "[mcp][server]") {
    services::RuntimeServices services;
    mcp::McpServer server(services);
    if (auto started = server.start(0); !started) {
        SKIP("This environment does not allow a loopback listener: " + started.error());
    }

    const Json message{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};

    // No header at all, a wrong token, and the right token under the wrong
    // scheme. The check runs before the method is even looked at, so none of
    // these should learn anything about what the server would have done.
    for (const auto& authorization : {std::string(), std::string("Bearer wrong"),
                                      "Basic " + server.token()}) {
        const auto response = post(server.port(), authorization, message.dump());
        REQUIRE_FALSE(response.empty());
        INFO("authorization: " << authorization);
        CHECK(response.find("HTTP/1.1 401") == 0);
        CHECK(response.find("tools") == std::string::npos);
    }

    server.stop();
}

TEST_CASE("A notification over the socket is accepted with no body", "[mcp][server]") {
    services::RuntimeServices services;
    mcp::McpServer server(services);
    if (auto started = server.start(0); !started) {
        SKIP("This environment does not allow a loopback listener: " + started.error());
    }

    const Json message{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    const auto response = post(server.port(), "Bearer " + server.token(), message.dump());
    REQUIRE_FALSE(response.empty());
    CHECK(response.find("HTTP/1.1 202") == 0);
    CHECK(bodyOf(response).empty());

    server.stop();
}

TEST_CASE("The server can be started again after being stopped", "[mcp][server]") {
    services::RuntimeServices services;
    mcp::McpServer server(services);

    if (auto started = server.start(0); !started) {
        SKIP("This environment does not allow a loopback listener: " + started.error());
    }
    const auto firstToken = server.token();
    server.stop();
    // A stopped server holds no token, or a client could keep using the old one
    // against the next session.
    CHECK(server.token().empty());

    REQUIRE(server.start(0).has_value());
    CHECK(server.token() != firstToken);
    CHECK(server.running());
    server.stop();
}

TEST_CASE("Starting a running server is refused rather than leaking the listener", "[mcp][server]") {
    services::RuntimeServices services;
    mcp::McpServer server(services);
    if (auto started = server.start(0); !started) {
        SKIP("This environment does not allow a loopback listener: " + started.error());
    }
    CHECK_FALSE(server.start(0).has_value());
    server.stop();
}
