#pragma once

// The MCP tool surface.
//
// This is the second programmatic client of the engines, after the Lua console,
// and it is deliberately built the same way: one registry of named handlers over
// services::RuntimeServices, each returning infra::Result so a failure arrives
// as a sentence rather than as an exception. Where a Lua function already exists
// for a job, the tool takes the same name and the same argument order -- those
// names are a published contract for the life of 3.x, and having the agent-facing
// vocabulary disagree with the scripting one would double the thing a reader has
// to learn for no gain.
//
// The layer sits beside scripting/ rather than under services/: it holds a
// RuntimeServices by reference and is owned by whatever drives it, exactly as
// LuaConsole is. It is not an engine and must not be mistaken for one.

#include "infra/Result.h"
#include "services/RuntimeServices.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ire::mcp {

using Json = nlohmann::json;

// One tool: what it is called, what it does, the shape of its arguments, and the
// code behind it.
struct Tool {
    std::string name;
    std::string description;
    Json inputSchema;
    std::function<infra::Result<Json>(const Json&)> handler;
    // Whether the handler changes engine state, and therefore has to run on the
    // UI thread rather than on the server's. Read-only tools go straight to the
    // mutex-guarded snapshot accessors, which is what they are for.
    bool mutating{};
};

class ToolRegistry {
public:
    explicit ToolRegistry(services::RuntimeServices& services);

    [[nodiscard]] const std::vector<Tool>& tools() const { return tools_; }
    [[nodiscard]] const Tool* find(const std::string& name) const;

    // Runs a tool by name. A mutating tool is marshalled onto the UI thread via
    // services::uiCommands(); with no window -- the tests, a headless embedder --
    // it runs inline, which is safe because the server serialises calls.
    infra::Result<Json> call(const std::string& name, const Json& arguments);

private:
    // One per group of engines, so no single translation unit holds the whole
    // surface. Defined across McpTools.cpp, McpToolsAnalysis.cpp and
    // McpToolsControl.cpp.
    void registerTarget();
    void registerMemory();
    void registerScan();
    void registerSymbols();
    void registerPointers();
    void registerAddressList();
    void registerStructures();
    void registerCode();
    void registerBreakpoints();
    void registerPatches();
    void registerSpeed();
    void registerProject();
    void registerWindow();

    void add(Tool tool);

    services::RuntimeServices& services_;
    std::vector<Tool> tools_;
    std::map<std::string, std::size_t> byName_;
};

} // namespace ire::mcp
