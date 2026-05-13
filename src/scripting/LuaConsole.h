#pragma once

#include "services/RuntimeServices.h"

#include <lua.hpp>

#include <string>
#include <vector>

namespace ire::scripting {

class LuaConsole {
public:
    explicit LuaConsole(services::RuntimeServices& services);
    ~LuaConsole();

    LuaConsole(const LuaConsole&) = delete;
    LuaConsole& operator=(const LuaConsole&) = delete;

    std::vector<std::string> run(const std::string& code);

private:
    void registerApi();
    void appendOutput(std::string text);

    static LuaConsole* self(lua_State* state);
    static int l_print(lua_State* state);
    static int l_processes(lua_State* state);
    static int l_attach(lua_State* state);
    static int l_detach(lua_State* state);
    static int l_read_u32(lua_State* state);
    static int l_write_u32(lua_State* state);
    static int l_read_bytes(lua_State* state);
    static int l_write_bytes(lua_State* state);
    static int l_scan_exact_i32(lua_State* state);
    static int l_scan_unknown_i32(lua_State* state);
    static int l_add_address(lua_State* state);
    static int l_alloc(lua_State* state);
    static int l_thread(lua_State* state);
    static int l_loadlibrary(lua_State* state);

    services::RuntimeServices& services_;
    lua_State* state_{};
    std::vector<std::string> output_;
};

} // namespace ire::scripting

