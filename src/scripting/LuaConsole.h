#pragma once

#include "services/RuntimeServices.h"

#include <lua.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ire::scripting {

// The scripting console.
//
// Scripts run on a worker thread rather than inline on the UI thread, and a
// count hook checks a cancel flag as they go. Running them inline meant a
// script as ordinary as "while true do end" wedged the whole application with
// no way back except killing it.
class LuaConsole {
public:
    explicit LuaConsole(services::RuntimeServices& services);
    ~LuaConsole();

    LuaConsole(const LuaConsole&) = delete;
    LuaConsole& operator=(const LuaConsole&) = delete;

    // Starts code on the worker thread. Returns false if a script is already
    // running, since one Lua state cannot be in two calls at once.
    bool submit(const std::string& code);
    [[nodiscard]] bool running() const { return running_; }
    // Asks the running script to stop. It unwinds at its next hook check.
    void cancel();

    // Output produced so far, removed from the buffer as it is taken. Safe to
    // call while a script is still running, so print() appears live.
    [[nodiscard]] std::vector<std::string> takeOutput();

private:
    void execute(std::string code);
    void registerApi();
    void applySandbox();
    void appendOutput(std::string text);

    static LuaConsole* self(lua_State* state);
    static LuaConsole* fromState(lua_State* state);
    static void countHook(lua_State* state, lua_Debug* debug);
    static int traceback(lua_State* state);

    static int l_print(lua_State* state);
    static int l_processes(lua_State* state);
    static int l_attach(lua_State* state);
    static int l_detach(lua_State* state);
    static int l_modules(lua_State* state);
    static int l_regions(lua_State* state);
    static int l_read(lua_State* state);
    static int l_write(lua_State* state);
    static int l_read_u32(lua_State* state);
    static int l_write_u32(lua_State* state);
    static int l_read_bytes(lua_State* state);
    static int l_write_bytes(lua_State* state);
    static int l_scan_exact(lua_State* state);
    static int l_scan_unknown(lua_State* state);
    static int l_scan_next(lua_State* state);
    static int l_scan_status(lua_State* state);
    static int l_scan_wait(lua_State* state);
    static int l_cancelled(lua_State* state);
    static int l_check_cancel(lua_State* state);
    static int l_scan_results(lua_State* state);
    static int l_resolve(lua_State* state);
    static int l_add_address(lua_State* state);
    static int l_alloc(lua_State* state);
    static int l_thread(lua_State* state);
    static int l_loadlibrary(lua_State* state);

    services::RuntimeServices& services_;
    lua_State* state_{};

    mutable std::mutex mutex_;
    std::vector<std::string> output_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
};

} // namespace ire::scripting
