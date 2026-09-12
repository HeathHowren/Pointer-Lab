#pragma once

#include <lua.hpp>

namespace ire::scripting {

// What every Lua state in Pointer Lab has taken away from it before a script
// runs, in one place so the console and the Lua Scanner cannot drift apart.
//
// Pointer Lab scripts exist to inspect and edit process memory. Nothing in that
// job needs to touch the file system, spawn programs or load native modules,
// and leaving those exposed turns a pasted script -- or a script record in a
// shared project file -- into arbitrary code execution on the machine. The
// scanner used to open the whole standard library and apply none of this.
//
// Removed: the `io` and `package` globals and their entries in the loaded-
// module registry (nil-ing the global alone left `io` one
// `debug.getregistry()` away), `require`, `dofile`, `loadfile`,
// `debug.getregistry`, and from `os` everything that is not about time.
//
// This is a guard against a careless script, not a security boundary: `load`
// stays, and a memory-editing tool hands a script the ability to write
// arbitrary bytes into another process regardless.
void applySandbox(lua_State* state);

// How many VM instructions run between cancel checks in a count hook. Small
// enough that a runaway loop stops the moment it is asked, large enough not to
// matter.
inline constexpr int sandboxHookInterval = 10000;

} // namespace ire::scripting
