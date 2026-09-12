#include "scripting/LuaSandbox.h"

#include <initializer_list>

namespace ire::scripting {

namespace {

void clearGlobal(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setglobal(state, name);
}

void clearField(lua_State* state, const char* table, const char* field) {
    lua_getglobal(state, table);
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        lua_setfield(state, -2, field);
    }
    lua_pop(state, 1);
}

// The registry's table of loaded modules is where `require` caches what it has
// opened, and every standard library is registered there by luaL_openlibs.
// Clearing the global and not this left the module itself reachable.
void clearLoadedModule(lua_State* state, const char* name) {
    luaL_getsubtable(state, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_pushnil(state);
    lua_setfield(state, -2, name);
    lua_pop(state, 1);
}

} // namespace

void applySandbox(lua_State* state) {
    clearGlobal(state, "io");
    clearGlobal(state, "package");
    clearGlobal(state, "require");
    clearGlobal(state, "dofile");
    clearGlobal(state, "loadfile");
    clearLoadedModule(state, "io");
    clearLoadedModule(state, "package");

    // debug stays, for tracebacks and introspection, but its way back into the
    // registry -- and through it to the loaded modules -- does not.
    clearField(state, "debug", "getregistry");

    // os keeps only the parts that report time.
    for (const char* removed : {"execute", "remove", "rename", "tmpname", "exit", "getenv", "setlocale"}) {
        clearField(state, "os", removed);
    }
}

} // namespace ire::scripting
