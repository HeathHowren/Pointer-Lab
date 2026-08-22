#include "scripting/LuaConsole.h"

#include "domain/Domain.h"
#include "engine_pointer/PointerScanner.h"
#include "infra/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <sstream>

namespace ire::scripting {

namespace {

// How many VM instructions run between cancel checks. Small enough that a
// runaway loop stops the moment it is asked, large enough not to matter.
constexpr int hookInterval = 10000;

// Windows module names are case-insensitive, and the case a script passes a path
// in need not match what the loader reports.
bool sameFileName(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
}

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

void pushTyped(lua_State* state, domain::ValueType type, const std::vector<std::uint8_t>& bytes) {
    switch (type) {
    case domain::ValueType::Int8:   lua_pushinteger(state, unpack<std::int8_t>(bytes)); break;
    case domain::ValueType::UInt8:  lua_pushinteger(state, unpack<std::uint8_t>(bytes)); break;
    case domain::ValueType::Int16:  lua_pushinteger(state, unpack<std::int16_t>(bytes)); break;
    case domain::ValueType::UInt16: lua_pushinteger(state, unpack<std::uint16_t>(bytes)); break;
    case domain::ValueType::Int32:  lua_pushinteger(state, unpack<std::int32_t>(bytes)); break;
    case domain::ValueType::UInt32: lua_pushinteger(state, unpack<std::uint32_t>(bytes)); break;
    case domain::ValueType::Int64:  lua_pushinteger(state, unpack<std::int64_t>(bytes)); break;
    case domain::ValueType::UInt64:
        // Lua integers are signed 64-bit, so a huge unsigned value would wrap
        // to a negative number. A float keeps it approximately right instead.
        lua_pushnumber(state, static_cast<lua_Number>(unpack<std::uint64_t>(bytes)));
        break;
    case domain::ValueType::Float:  lua_pushnumber(state, unpack<float>(bytes)); break;
    case domain::ValueType::Double: lua_pushnumber(state, unpack<double>(bytes)); break;
    case domain::ValueType::Bytes:  lua_pushstring(state, domain::bytesToHex(bytes).c_str()); break;
    }
}

// Reads a Lua value into the byte representation of `type`. Every luaL_check
// call that could longjmp happens here, before any caller has built objects
// with destructors on the stack.
std::vector<std::uint8_t> packTyped(lua_State* state, int index, domain::ValueType type) {
    switch (type) {
    case domain::ValueType::Int8:   return pack(static_cast<std::int8_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::UInt8:  return pack(static_cast<std::uint8_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::Int16:  return pack(static_cast<std::int16_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::UInt16: return pack(static_cast<std::uint16_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::Int32:  return pack(static_cast<std::int32_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::UInt32: return pack(static_cast<std::uint32_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::Int64:  return pack(static_cast<std::int64_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::UInt64: return pack(static_cast<std::uint64_t>(luaL_checkinteger(state, index)));
    case domain::ValueType::Float:  return pack(static_cast<float>(luaL_checknumber(state, index)));
    case domain::ValueType::Double: return pack(static_cast<double>(luaL_checknumber(state, index)));
    case domain::ValueType::Bytes:  return domain::parseHexBytes(luaL_checkstring(state, index));
    }
    return {};
}

std::optional<domain::ScanMode> parseScanMode(const std::string& text) {
    if (text == "exact")     return domain::ScanMode::Exact;
    if (text == "unknown")   return domain::ScanMode::UnknownInitial;
    if (text == "changed")   return domain::ScanMode::Changed;
    if (text == "unchanged") return domain::ScanMode::Unchanged;
    if (text == "increased") return domain::ScanMode::Increased;
    if (text == "decreased") return domain::ScanMode::Decreased;
    return std::nullopt;
}

} // namespace

LuaConsole::LuaConsole(services::RuntimeServices& services) : services_(services) {
    state_ = luaL_newstate();
    luaL_openlibs(state_);
    // The extra space sits alongside the state, so the hook can find its way
    // back here without an upvalue.
    *static_cast<LuaConsole**>(lua_getextraspace(state_)) = this;
    applySandbox();
    registerApi();
}

LuaConsole::~LuaConsole() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (state_ != nullptr) {
        lua_close(state_);
    }
}

bool LuaConsole::submit(const std::string& code) {
    if (running_) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    cancel_ = false;
    running_ = true;
    worker_ = std::jthread([this, code]() mutable { execute(std::move(code)); });
    return true;
}

void LuaConsole::cancel() {
    cancel_ = true;
    // A script that started a scan and was then stopped used to leave the scan
    // running in the background, so Stop stopped the script but not the work it
    // had set going -- and the next script inherited its results.
    services_.scanJob().cancel();
}

std::vector<std::string> LuaConsole::takeOutput() {
    std::scoped_lock lock(mutex_);
    return std::move(output_);
}

void LuaConsole::execute(std::string code) {
    // The script runs on its own coroutine rather than directly on the main
    // state, and that is what makes Stop actually stop it.
    //
    // Cancelling used to raise a Lua error from the hook, which pcall catches
    // like any other. "while true do pcall(f) end" swallowed the cancel and kept
    // running, erroring again every 10 000 instructions forever -- and a runaway
    // loop is precisely what the Stop button exists for. A yield cannot be
    // caught: pcall passes it straight through to lua_resume. So the hook yields
    // instead, and a cancelled coroutine is simply never resumed.
    lua_State* thread = lua_newthread(state_);
    const int threadIndex = lua_gettop(state_);

    if (luaL_loadstring(thread, code.c_str()) != LUA_OK) {
        const char* message = lua_tostring(thread, -1);
        appendOutput(message != nullptr ? message : "The script could not be compiled.");
        lua_settop(state_, threadIndex - 1);
        running_ = false;
        return;
    }

    lua_sethook(thread, &LuaConsole::countHook, LUA_MASKCOUNT, hookInterval);

    int results = 0;
    const int status = lua_resume(thread, state_, 0, &results);

    if (status == LUA_YIELD) {
        // Nothing in the API yields, so this is the cancel hook and only the
        // cancel hook.
        appendOutput("Script cancelled.");
    } else if (status != LUA_OK) {
        const char* message = lua_tostring(thread, -1);
        // Built against the coroutine's stack, or the traceback describes this
        // function instead of the script that actually failed.
        luaL_traceback(state_, thread, message != nullptr ? message : "Script failed with a non-string error.", 0);
        const char* trace = lua_tostring(state_, -1);
        appendOutput(trace != nullptr ? trace : "Script failed with a non-string error.");
        lua_pop(state_, 1);
    }

    // Drops the coroutine along with anything the chunk returned. A cancelled
    // one is left suspended and collected; it is never resumed again.
    lua_settop(state_, threadIndex - 1);
    running_ = false;
}

void LuaConsole::countHook(lua_State* state, lua_Debug*) {
    auto* console = fromState(state);
    if (console != nullptr && console->cancel_.load(std::memory_order_relaxed)) {
        // Yields rather than raising, so a pcall in the script cannot swallow
        // it. lua_yield does not return: it unwinds to lua_resume in execute().
        lua_yield(state, 0);
    }
}

int LuaConsole::traceback(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    if (message == nullptr) {
        if (luaL_callmeta(state, 1, "__tostring") != 0 && lua_type(state, -1) == LUA_TSTRING) {
            return 1;
        }
        message = lua_pushfstring(state, "(error object is a %s value)", luaL_typename(state, 1));
    }
    luaL_traceback(state, state, message, 1);
    return 1;
}

void LuaConsole::applySandbox() {
    // Pointer Lab scripts exist to inspect and edit process memory. Nothing in
    // that job needs to touch the file system, spawn programs or load native
    // modules, and leaving those exposed turns a pasted script into arbitrary
    // code execution on the machine.
    clearGlobal(state_, "io");
    clearGlobal(state_, "package");
    clearGlobal(state_, "require");
    clearGlobal(state_, "dofile");
    clearGlobal(state_, "loadfile");

    // os keeps only the parts that report time.
    for (const char* removed : {"execute", "remove", "rename", "tmpname", "exit", "getenv", "setlocale"}) {
        clearField(state_, "os", removed);
    }
}

void LuaConsole::registerApi() {
    pushFunction(state_, this, "print", &LuaConsole::l_print);
    pushFunction(state_, this, "processes", &LuaConsole::l_processes);
    pushFunction(state_, this, "attach", &LuaConsole::l_attach);
    pushFunction(state_, this, "detach", &LuaConsole::l_detach);
    pushFunction(state_, this, "modules", &LuaConsole::l_modules);
    pushFunction(state_, this, "regions", &LuaConsole::l_regions);
    pushFunction(state_, this, "read", &LuaConsole::l_read);
    pushFunction(state_, this, "write", &LuaConsole::l_write);
    pushFunction(state_, this, "read_u32", &LuaConsole::l_read_u32);
    pushFunction(state_, this, "write_u32", &LuaConsole::l_write_u32);
    pushFunction(state_, this, "read_bytes", &LuaConsole::l_read_bytes);
    pushFunction(state_, this, "write_bytes", &LuaConsole::l_write_bytes);
    pushFunction(state_, this, "scan_exact", &LuaConsole::l_scan_exact);
    pushFunction(state_, this, "scan_unknown", &LuaConsole::l_scan_unknown);
    pushFunction(state_, this, "scan_next", &LuaConsole::l_scan_next);
    pushFunction(state_, this, "scan_status", &LuaConsole::l_scan_status);
    pushFunction(state_, this, "scan_wait", &LuaConsole::l_scan_wait);
    pushFunction(state_, this, "scan_results", &LuaConsole::l_scan_results);
    pushFunction(state_, this, "cancelled", &LuaConsole::l_cancelled);
    pushFunction(state_, this, "check_cancel", &LuaConsole::l_check_cancel);
    pushFunction(state_, this, "resolve", &LuaConsole::l_resolve);
    pushFunction(state_, this, "add_address", &LuaConsole::l_add_address);
    pushFunction(state_, this, "alloc", &LuaConsole::l_alloc);
    pushFunction(state_, this, "thread", &LuaConsole::l_thread);
    pushFunction(state_, this, "loadlibrary", &LuaConsole::l_loadlibrary);
}

void LuaConsole::appendOutput(std::string text) {
    std::scoped_lock lock(mutex_);
    // A script printing in a tight loop would otherwise grow this without
    // bound; the console only ever shows the tail anyway.
    if (output_.size() >= 10000) {
        output_.erase(output_.begin(), output_.begin() + 5000);
        output_.emplace_back("... earlier output discarded ...");
    }
    output_.push_back(std::move(text));
}

LuaConsole* LuaConsole::self(lua_State* state) {
    return static_cast<LuaConsole*>(lua_touserdata(state, lua_upvalueindex(1)));
}

LuaConsole* LuaConsole::fromState(lua_State* state) {
    return *static_cast<LuaConsole**>(lua_getextraspace(state));
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

int LuaConsole::l_modules(lua_State* state) {
    auto* console = self(state);
    const auto modules = console->services_.session().modules();
    lua_newtable(state);
    int index = 1;
    for (const auto& module : modules) {
        lua_newtable(state);
        lua_pushstring(state, domain::narrow(module.name).c_str());
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, static_cast<lua_Integer>(module.base));
        lua_setfield(state, -2, "base");
        lua_pushinteger(state, static_cast<lua_Integer>(module.size));
        lua_setfield(state, -2, "size");
        lua_rawseti(state, -2, index++);
    }
    return 1;
}

int LuaConsole::l_regions(lua_State* state) {
    auto* console = self(state);
    const auto regions = console->services_.session().regions();
    lua_newtable(state);
    int index = 1;
    for (const auto& region : regions) {
        lua_newtable(state);
        lua_pushinteger(state, static_cast<lua_Integer>(region.base));
        lua_setfield(state, -2, "base");
        lua_pushinteger(state, static_cast<lua_Integer>(region.size));
        lua_setfield(state, -2, "size");
        lua_pushboolean(state, region.readable);
        lua_setfield(state, -2, "readable");
        lua_pushboolean(state, region.writable);
        lua_setfield(state, -2, "writable");
        lua_pushboolean(state, region.executable);
        lua_setfield(state, -2, "executable");
        lua_rawseti(state, -2, index++);
    }
    return 1;
}

int LuaConsole::l_read(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const char* typeText = luaL_optstring(state, 2, "i32");
    const auto type = domain::parseValueType(typeText);
    if (!type) {
        return luaL_error(state, "Unknown value type '%s'.", typeText);
    }
    const auto size = std::max<std::size_t>(1, domain::valueTypeSize(*type));

    auto bytes = console->services_.session().readBytes(address, size);
    if (!bytes || bytes.value().size() != size) {
        lua_pushnil(state);
        lua_pushstring(state, bytes ? "Short read." : bytes.error().c_str());
        return 2;
    }
    pushTyped(state, *type, bytes.value());
    return 1;
}

int LuaConsole::l_write(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const char* typeText = luaL_checkstring(state, 2);
    const auto type = domain::parseValueType(typeText);
    if (!type) {
        return luaL_error(state, "Unknown value type '%s'.", typeText);
    }

    // Done before anything with a destructor exists, because a failed check
    // longjmps straight out of this function.
    auto bytes = packTyped(state, 3, *type);
    auto result = console->services_.session().writeBytes(address, bytes);
    lua_pushboolean(state, result.has_value());
    if (!result) {
        lua_pushstring(state, result.error().c_str());
        return 2;
    }
    return 1;
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

int LuaConsole::l_scan_exact(lua_State* state) {
    auto* console = self(state);
    const char* typeText = luaL_optstring(state, 2, "i32");
    const auto type = domain::parseValueType(typeText);
    if (!type) {
        return luaL_error(state, "Unknown value type '%s'.", typeText);
    }

    domain::ScanValue value;
    value.type = *type;
    value.bytes = packTyped(state, 1, *type);
    console->services_.scanJob().startFirst(domain::ScanMode::Exact, std::move(value));
    return 0;
}

int LuaConsole::l_scan_unknown(lua_State* state) {
    auto* console = self(state);
    const char* typeText = luaL_optstring(state, 1, "i32");
    const auto type = domain::parseValueType(typeText);
    if (!type) {
        return luaL_error(state, "Unknown value type '%s'.", typeText);
    }

    domain::ScanValue value;
    value.type = *type;
    value.bytes.assign(std::max<std::size_t>(1, domain::valueTypeSize(*type)), 0);
    console->services_.scanJob().startFirst(domain::ScanMode::UnknownInitial, std::move(value));
    return 0;
}

int LuaConsole::l_scan_next(lua_State* state) {
    auto* console = self(state);
    const char* modeText = luaL_checkstring(state, 1);
    const auto mode = parseScanMode(modeText);
    if (!mode) {
        return luaL_error(
            state, "Unknown scan mode '%s'. Use exact, unknown, changed, unchanged, increased or decreased.",
            modeText);
    }
    const auto type = console->services_.scanJob().valueType();

    domain::ScanValue value;
    value.type = type;
    if (*mode == domain::ScanMode::Exact) {
        value.bytes = packTyped(state, 2, type);
    } else {
        value.bytes.assign(std::max<std::size_t>(1, domain::valueTypeSize(type)), 0);
    }

    auto previous = console->services_.scanJob().results();
    if (previous.empty()) {
        lua_pushboolean(state, 0);
        lua_pushstring(state, "There are no results to narrow. Run scan_exact or scan_unknown first.");
        return 2;
    }
    console->services_.scanJob().startNext(*mode, std::move(value), std::move(previous));
    lua_pushboolean(state, 1);
    return 1;
}

int LuaConsole::l_scan_status(lua_State* state) {
    auto* console = self(state);
    const auto progress = console->services_.scanJob().progress();
    lua_newtable(state);
    lua_pushboolean(state, progress.running);
    lua_setfield(state, -2, "running");
    lua_pushinteger(state, static_cast<lua_Integer>(progress.results));
    lua_setfield(state, -2, "results");
    lua_pushnumber(state, progress.fraction);
    lua_setfield(state, -2, "fraction");
    lua_pushboolean(state, progress.truncated);
    lua_setfield(state, -2, "truncated");
    lua_pushstring(state, progress.status.c_str());
    lua_setfield(state, -2, "status");
    return 1;
}

int LuaConsole::l_cancelled(lua_State* state) {
    auto* console = self(state);
    lua_pushboolean(state, console->cancel_.load(std::memory_order_relaxed) ? 1 : 0);
    return 1;
}

int LuaConsole::l_check_cancel(lua_State* state) {
    auto* console = self(state);
    if (console->cancel_.load(std::memory_order_relaxed)) {
        // Same mechanism as the hook, so this cannot be caught either. A script
        // that could pcall its way past its own cancellation check would make
        // the function pointless.
        lua_yield(state, 0);
    }
    return 0;
}

int LuaConsole::l_scan_wait(lua_State* state) {
    auto* console = self(state);
    const auto timeoutMs = static_cast<long long>(luaL_optinteger(state, 1, 60000));

    // Blocking here is only safe because scripts no longer run on the UI
    // thread. Cancelling still works: the flag is checked on the way round.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (console->services_.scanJob().progress().running) {
        if (console->cancel_.load(std::memory_order_relaxed)) {
            // Yields rather than raising, for the same reason the hook does: an
            // error here is catchable, and a script that pcalls around its wait
            // would carry on running after the user pressed Stop.
            lua_yield(state, 0);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            lua_pushboolean(state, 0);
            lua_pushstring(state, "Timed out waiting for the scan.");
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    lua_pushboolean(state, 1);
    return 1;
}

int LuaConsole::l_scan_results(lua_State* state) {
    auto* console = self(state);
    const auto limit = static_cast<std::size_t>(luaL_optinteger(state, 1, 1000));

    const auto results = console->services_.scanJob().results();
    const auto type = console->services_.scanJob().valueType();
    const auto count = std::min(limit, results.size());

    lua_newtable(state);
    for (std::size_t i = 0; i < count; ++i) {
        lua_newtable(state);
        lua_pushinteger(state, static_cast<lua_Integer>(results[i].address));
        lua_setfield(state, -2, "address");
        pushTyped(state, type, results[i].current);
        lua_setfield(state, -2, "value");
        lua_pushstring(state, domain::bytesToHex(results[i].current, false).c_str());
        lua_setfield(state, -2, "hex");
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }
    // Second return value so a script can tell a truncated list from a
    // complete one instead of assuming it saw everything.
    lua_pushinteger(state, static_cast<lua_Integer>(results.size()));
    return 2;
}

int LuaConsole::l_resolve(lua_State* state) {
    auto* console = self(state);
    const char* moduleName = luaL_checkstring(state, 1);
    const auto moduleOffset = static_cast<std::uintptr_t>(luaL_checkinteger(state, 2));
    luaL_checktype(state, 3, LUA_TTABLE);

    domain::PointerChain chain;
    chain.moduleName = domain::widen(moduleName);
    chain.moduleOffset = moduleOffset;

    const auto length = static_cast<std::size_t>(lua_rawlen(state, 3));
    for (std::size_t i = 1; i <= length; ++i) {
        lua_rawgeti(state, 3, static_cast<lua_Integer>(i));
        chain.offsets.push_back(static_cast<std::ptrdiff_t>(lua_tointeger(state, -1)));
        lua_pop(state, 1);
    }

    auto resolved = engine_pointer::resolveChain(console->services_.session(), chain);
    if (!resolved) {
        lua_pushnil(state);
        lua_pushstring(state, resolved.error().c_str());
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(resolved.value()));
    return 1;
}

int LuaConsole::l_add_address(lua_State* state) {
    auto* console = self(state);
    const auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
    const char* typeText = luaL_optstring(state, 2, "i32");
    const char* description = luaL_optstring(state, 3, "Lua entry");
    const char* group = luaL_optstring(state, 4, "Lua");
    // Falling back to i32 here would give the entry the wrong width and read
    // and write the wrong number of bytes at that address, silently. Every
    // other function that takes a type name raises; this one used not to.
    const auto type = domain::parseValueType(typeText);
    if (!type) {
        return luaL_error(state, "Unknown value type '%s'.", typeText);
    }
    const auto id = console->services_.addressList().add(address, *type, description, group);
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
    const auto wide = domain::widen(path);

    auto result = console->services_.injector().loadLibrary(wide);
    lua_pushboolean(state, result.has_value());
    if (!result) {
        lua_pushstring(state, result.error().c_str());
        return 2;
    }

    // The remote thread's exit code is only the low 32 bits of the module
    // handle, so on a 64-bit target it is a truncated value that looks like an
    // address but is not one. The module list knows the real base; refreshing it
    // is also what makes the newly loaded DLL visible to modules() at all.
    console->services_.session().refresh();
    const auto fileName = std::filesystem::path(wide).filename().wstring();
    for (const auto& module : console->services_.session().modules()) {
        if (sameFileName(module.name, fileName)) {
            lua_pushinteger(state, static_cast<lua_Integer>(module.base));
            return 2;
        }
    }

    // The DLL did load; we just could not find it afterwards. Report the exit
    // code rather than claiming a failure that did not happen.
    lua_pushinteger(state, result.value());
    return 2;
}

} // namespace ire::scripting
