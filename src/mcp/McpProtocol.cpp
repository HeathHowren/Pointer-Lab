#include "mcp/McpProtocol.h"

#include <Version.h>

#include <exception>

namespace ire::mcp {

namespace {

// The JSON-RPC codes. Spelled out rather than inlined because the difference
// between -32601 and -32602 is the difference between "you asked for something
// that does not exist" and "you asked correctly but wrongly", and a client
// distinguishes them.
constexpr int parseError = -32700;
constexpr int invalidRequest = -32600;
constexpr int methodNotFound = -32601;
constexpr int internalError = -32603;

Json errorResponse(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"},
                {"id", id.is_null() ? Json(nullptr) : id},
                {"error", Json{{"code", code}, {"message", message}}}};
}

Json okResponse(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

} // namespace

std::string Protocol::handle(const std::string& body) {
    Json request;
    try {
        request = Json::parse(body);
    } catch (const std::exception& error) {
        return errorResponse(Json(nullptr), parseError, std::string("Invalid JSON: ") + error.what()).dump();
    }

    // A batch is an array. MCP dropped batching after 2025-03-26, but a client
    // that still sends one gets an answer rather than a parse error.
    if (request.is_array()) {
        if (request.empty()) {
            return errorResponse(Json(nullptr), invalidRequest, "A batch must not be empty.").dump();
        }
        Json responses = Json::array();
        for (const auto& entry : request) {
            auto response = dispatch(entry);
            if (!response.is_null()) {
                responses.push_back(std::move(response));
            }
        }
        // A batch made entirely of notifications gets no reply at all.
        return responses.empty() ? std::string() : responses.dump();
    }

    auto response = dispatch(request);
    return response.is_null() ? std::string() : response.dump();
}

Json Protocol::dispatch(const Json& request) {
    if (!request.is_object()) {
        return errorResponse(Json(nullptr), invalidRequest, "A request must be a JSON object.");
    }

    const Json id = request.contains("id") ? request.at("id") : Json(nullptr);
    // No id means a notification, and a notification takes no reply -- not even
    // an error one. Answering `notifications/initialized` with a method-not-found
    // is the single most common way to make a client hang up.
    const bool notification = !request.contains("id") || request.at("id").is_null();

    if (!request.contains("method") || !request.at("method").is_string()) {
        return notification ? Json(nullptr) : errorResponse(id, invalidRequest, "method is required.");
    }
    const auto method = request.at("method").get<std::string>();
    const Json params = request.contains("params") ? request.at("params") : Json::object();

    try {
        if (method == "initialize") {
            // Echo a version we know, otherwise offer ours. The handshake is not
            // worth failing over.
            std::string version = protocolVersion;
            if (params.is_object() && params.contains("protocolVersion") &&
                params.at("protocolVersion").is_string()) {
                version = params.at("protocolVersion").get<std::string>();
            }
            return okResponse(id, Json{{"protocolVersion", version},
                                       {"capabilities", Json{{"tools", Json{{"listChanged", false}}}}},
                                       {"serverInfo", Json{{"name", "pointerlab"},
                                                           {"title", "Pointer Lab"},
                                                           {"version", POINTERLAB_VERSION_STRING}}},
                                       {"instructions",
                                        "Drives a live Pointer Lab session: the same attached process, "
                                        "scan and address list the person at the window is looking at. "
                                        "Attach to a process first; everything else needs it. Scans run "
                                        "in the background, so poll scan_status before scan_results. "
                                        "Writes, patches and injection take effect immediately in a live "
                                        "process and are not undone when you disconnect."}});
        }
        if (method == "notifications/initialized" || method == "notifications/cancelled") {
            return Json(nullptr);
        }
        if (method == "ping") {
            return notification ? Json(nullptr) : okResponse(id, Json::object());
        }
        if (method == "tools/list") {
            Json list = Json::array();
            for (const auto& tool : tools_.tools()) {
                list.push_back(Json{{"name", tool.name},
                                    {"description", tool.description},
                                    {"inputSchema", tool.inputSchema}});
            }
            return okResponse(id, Json{{"tools", std::move(list)}});
        }
        if (method == "tools/call") {
            auto result = callTool(params);
            return notification ? Json(nullptr) : okResponse(id, std::move(result));
        }
        // resources and prompts are advertised as absent in the handshake, so a
        // well-behaved client never asks. One that does gets a real answer
        // rather than an error, because an empty list is the true one.
        if (method == "resources/list") {
            return okResponse(id, Json{{"resources", Json::array()}});
        }
        if (method == "prompts/list") {
            return okResponse(id, Json{{"prompts", Json::array()}});
        }
    } catch (const std::exception& error) {
        return notification ? Json(nullptr) : errorResponse(id, internalError, error.what());
    }

    if (notification) {
        return Json(nullptr);
    }
    return errorResponse(id, methodNotFound, "There is no method called \"" + method + "\".");
}

Json Protocol::callTool(const Json& params) {
    const auto fail = [](const std::string& message) {
        // A tool failure is a *successful* JSON-RPC call carrying isError, not a
        // protocol error. The distinction matters: a protocol error is the
        // client's bug and is not shown to the model, while this is information
        // the model is supposed to read and act on.
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", message}}})},
                    {"isError", true}};
    };

    if (!params.is_object() || !params.contains("name") || !params.at("name").is_string()) {
        return fail("tools/call needs a tool name.");
    }
    const auto name = params.at("name").get<std::string>();
    const Json arguments = params.contains("arguments") ? params.at("arguments") : Json::object();

    infra::Result<Json> outcome = infra::Result<Json>::fail("The tool did not run.");
    try {
        outcome = tools_.call(name, arguments);
    } catch (const std::exception& error) {
        // An engine that threw is a bug, but it is one the caller should hear
        // about as a tool failure rather than as a dropped connection.
        outcome = infra::Result<Json>::fail(std::string("The tool threw: ") + error.what());
    }

    if (logger_) {
        logger_(name + (outcome ? std::string(" ok") : " failed: " + outcome.error()));
    }

    if (!outcome) {
        // The Win32 code, when there is one, is the difference between "needs
        // elevation" and "that page is not mapped", and dropping it makes both
        // read as a flat refusal.
        std::string message = outcome.error();
        if (outcome.code() != 0) {
            message += " (Windows error " + std::to_string(outcome.code()) + ")";
        }
        return fail(message);
    }
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", outcome.value().dump(2)}}})},
                {"isError", false}};
}

} // namespace ire::mcp
