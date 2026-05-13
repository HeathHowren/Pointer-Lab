#include "scripting/LuaConsole.h"

#include "domain/Domain.h"
#include "infra/Logger.h"

#include <cstring>
#include <sstream>

namespace ire::scripting {

namespace {

template <typename T>
std::vector<std::uint8_t> pack(T value) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T>
T unpack(const std::vector<std::uint8_t>& bytes) {
    T value{};
    if (bytes.size() >= sizeof(T)) {
        std::memcpy(&value, bytes.data(), sizeof(T));
    }
    return value;
}

void pushFunction(lua_State* state, LuaConsole* console, const char* name, lua_CFunction fn) {
    lua_pushlightuserdata(state, console);
    lua_pushcclosure(state, fn, 1);
    lua_setglobal(state, name);
}

} // namespace

LuaConsole::LuaConsole(services::RuntimeServices& services) : services_(services) {
    state_ = luaL_newstate();
    luaL_openlibs(state_);
    registerApi();
}

LuaConsole::~LuaConsole() {
    if (state_) {
        lua_close(state_);
    }
}

std::vector<std::string> LuaConsole::run(const std::string& code) {
    output_.clear();
    const int load = luaL_loadstring(state_, code.c_str());
    if (load != LUA_OK) {
        appendOutput(lua_tostring(state_, -1));
        lua_pop(state_, 1);
        return output_;
    }

    const int call = lua_pcall(state_, 0, LUA_MULTRET, 0);
    if (call != LUA_OK) {
        appendOutput(lua_tostring(state_, -1));
        lua_pop(state_, 1);
    }
    return output_;
}

void LuaConsole::registerApi() {
    pushFunction(state_, this, "print", &LuaConsole::l_print);
    pushFunction(state_, this, "processes", &LuaConsole::l_processes);
    pushFunction(state_, this, "attach", &LuaConsole::l_attach);
    pushFunction(state_, this, "detach", &LuaConsole::l_detach);
    pushFunction(state_, this, "read_u32", &LuaConsole::l_read_u32);
    pushFunction(state_, this, "write_u32", &LuaConsole::l_write_u32);
    pushFunction(state_, this, "read_bytes", &LuaConsole::l_read_bytes);
    pushFunction(state_, this, "write_bytes", &LuaConsole::l_write_bytes);
    pushFunction(state_, this, "scan_exact_i32", &LuaConsole::l_scan_exact_i32);
    pushFunction(state_, this, "scan_unknown_i32", &LuaConsole::l_scan_unknown_i32);
    pushFunction(state_, this, "add_address", &LuaConsole::l_add_address);
    pushFunction(state_, this, "alloc", &LuaConsole::l_alloc);
    pushFunction(state_, this, "thread", &LuaConsole::l_thread);
    pushFunction(state_, this, "loadlibrary", &LuaConsole::l_loadlibrary);
}

void LuaConsole::appendOutput(std::string text) {
    output_.push_back(std::move(text));
}

LuaConsole* LuaConsole::self(lua_State* state) {
    return static_cast<LuaConsole*>(lua_touserdata(state, lua_upvalueindex(1)));
}

int LuaConsole::l_print(lua_State* state) {
    auto* console = self(state);
    std::ostringstream out;
    const int count = lua_gettop(state);
    for (int i = 1; i <= count; ++i) {
        if (i > 1) {
            out << '\t';
        }
        size_t len{};
        const char* text = luaL_tolstring(state, i, &len);
        out.write(text, static_cast<std::streamsize>(len));
        lua_pop(state, 1);
    }
    console->appendOutput(out.str());
    return 0;
}

int LuaConsole::l_processes(lua_State* state) {
    auto* console = self(state);
    const auto processes = console->services_.platform().listProcesses();
    lua_newtable(state);
    int index = 1;
    for (const auto& process : processes) {
        lua_newtable(state);
        lua_pushinteger(state, process.pid);
        lua_setfield(state, -2, "pid");
        lua_pushstring(state, domain::narrow(process.name).c_str());
        lua_setfield(state, -2, "name");
        lua_rawseti(state, -2, index++);
    }
    return 1;
}

int LuaConsole::l_attach(lua_State* state) {
    auto* console = self(state);
    const auto pid = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
    auto result = console->services_.session().attach(pid);
    lua_pushboolean(state, result.has_value());
    if (!result) {
        lua_pushstring(state, result.error().c_str());
        return 2;
    }
    return 1;
}

int LuaConsole::l_detach(lua_State* state) {
    self(state)->services_.session().detach();
    return 0;
}

int LuaConsole::l_read_u32(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    auto bytes = console->services_.session().readBytes(address, sizeof(std::uint32_t));
    if (!bytes || bytes.value().size() != sizeof(std::uint32_t)) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushinteger(state, unpack<std::uint32_t>(bytes.value()));
    return 1;
}

int LuaConsole::l_write_u32(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const auto value = static_cast<std::uint32_t>(luaL_checkinteger(state, 2));
    auto result = console->services_.session().writeBytes(address, pack(value));
    lua_pushboolean(state, result.has_value());
    return 1;
}

int LuaConsole::l_read_bytes(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const auto size = static_cast<std::size_t>(luaL_checkinteger(state, 2));
    auto bytes = console->services_.session().readBytes(address, size);
    if (!bytes) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushstring(state, domain::bytesToHex(bytes.value()).c_str());
    return 1;
}

int LuaConsole::l_write_bytes(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const char* text = luaL_checkstring(state, 2);
    auto bytes = domain::parseHexBytes(text);
    auto result = console->services_.session().writeBytes(address, bytes);
    lua_pushboolean(state, result.has_value());
    return 1;
}

int LuaConsole::l_scan_exact_i32(lua_State* state) {
    auto* console = self(state);
    const auto value = static_cast<std::int32_t>(luaL_checkinteger(state, 1));
    domain::ScanValue scanValue;
    scanValue.type = domain::ValueType::Int32;
    scanValue.bytes = pack(value);
    scanValue.text = std::to_string(value);
    console->services_.scanJob().startFirst(domain::ScanMode::Exact, scanValue);
    return 0;
}

int LuaConsole::l_scan_unknown_i32(lua_State* state) {
    auto* console = self(state);
    domain::ScanValue scanValue;
    scanValue.type = domain::ValueType::Int32;
    scanValue.bytes = pack(std::int32_t{0});
    scanValue.text = "unknown";
    console->services_.scanJob().startFirst(domain::ScanMode::UnknownInitial, scanValue);
    return 0;
}

int LuaConsole::l_add_address(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const char* typeText = luaL_optstring(state, 2, "i32");
    const char* description = luaL_optstring(state, 3, "Lua entry");
    const char* group = luaL_optstring(state, 4, "Lua");
    const auto type = domain::parseValueType(typeText).value_or(domain::ValueType::Int32);
    const auto id = console->services_.addressList().add(address, type, description, group);
    lua_pushinteger(state, static_cast<lua_Integer>(id));
    return 1;
}

int LuaConsole::l_alloc(lua_State* state) {
    auto* console = self(state);
    const auto size = static_cast<std::size_t>(luaL_checkinteger(state, 1));
    auto result = console->services_.injector().allocate(size, PAGE_EXECUTE_READWRITE);
    if (!result) {
        lua_pushnil(state);
        lua_pushstring(state, result.error().c_str());
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(result.value()));
    return 1;
}

int LuaConsole::l_thread(lua_State* state) {
    auto* console = self(state);
    const auto start = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const auto parameter = static_cast<std::uintptr_t>(luaL_optinteger(state, 2, 0));
    auto result = console->services_.injector().createThread(start, parameter);
    lua_pushboolean(state, result.has_value());
    if (result) {
        lua_pushinteger(state, result.value());
        return 2;
    }
    lua_pushstring(state, result.error().c_str());
    return 2;
}

int LuaConsole::l_loadlibrary(lua_State* state) {
    auto* console = self(state);
    const char* path = luaL_checkstring(state, 1);
    auto result = console->services_.injector().loadLibrary(domain::widen(path));
    lua_pushboolean(state, result.has_value());
    if (result) {
        lua_pushinteger(state, result.value());
        return 2;
    }
    lua_pushstring(state, result.error().c_str());
    return 2;
}

} // namespace ire::scripting

