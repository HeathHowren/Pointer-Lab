# Lua API reference

The Lua panel embeds Lua 5.4 with the functions below available as plain
globals. Everything here is implemented in
[`src/scripting/LuaConsole.cpp`](../src/scripting/LuaConsole.cpp).

There is **one Lua state for the whole session**, so a global you set in one run
is still there on the next. Scripts run on a worker thread, one at a time, each
on its own coroutine; a second submission while one is running is refused with
"A script is already running."

The Lua *Scanner* panel is a separate feature with its own `function(ctx)`
predicate API. None of the functions below relate to it.

## Conventions

**Value type names**, case-insensitive: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`,
`i64`, `u64`, `f32`, `f64`, `bytes`. Anything else is rejected with an error.

**Addresses** are Lua integers in both directions.

**Returned values** are Lua integers, with three exceptions. `f32` and `f64`
come back as Lua *numbers*, as you would expect. `u64` also comes back as a
number rather than an integer, because a full-range unsigned 64-bit value would
wrap negative in a signed Lua integer. `bytes` comes back as an uppercase hex
string.

**Errors** arrive one of three ways, noted per function:

- a raised Lua error, catchable with `pcall`
- `nil` plus a message
- `false` plus a message

**Hex input** (`write(..., "bytes", ...)`, `write_bytes`) is parsed leniently:
every non-hex character is discarded, and an odd number of digits is left-padded
with `0`. `"48 8B 05"`, `"488b05"` and `"48-8B-05"` are the same input.

## Output

### `print(...)`

Replaces the stock `print`. Converts each argument with `tostring` semantics
(honouring `__tostring`), joins with tabs, and appends one line to the console.
Output appears while the script is still running.

The console keeps the last 10 000 lines. On overflow the oldest 5 000 are
dropped and a `... earlier output discarded ...` marker is inserted.

## Target

### `processes()` → table

Array of `{ pid = integer, name = string }`, sorted case-insensitively by name.
Never fails; returns an empty table if the snapshot could not be taken.

### `attach(pid)` → boolean [, string]

Attaches to `pid`. Returns `true`, or `false, message`.

Attaching with read-only access still returns `true` — the degraded-access
warning goes to the log and the UI, not to Lua. If your script needs to write,
check that a write succeeds rather than trusting the attach.

Raises if `pid` is not an integer.

### `detach()`

Always succeeds, including when nothing is attached. Clears the cached module
and region lists.

### `modules()` → table

Array of `{ name = string, base = integer, size = integer }`. Full paths are not
exposed.

This reads the snapshot taken when you attached, refreshed only by the UI's
Refresh actions — so it is empty when nothing is attached, and will not show a
module the target loaded after you attached.

### `regions()` → table

Array of `{ base, size, readable, writable, executable }` — three integers and
three booleans. Same snapshot caveat as `modules()`.

## Memory

### `read(address, type = "i32")` → value | nil, string

Reads and decodes one value. Returns the value, or `nil, message` where the
message is either the OS error or `"Short read."` when the read succeeded but
returned fewer bytes than the type needs.

Raises on an unrecognised type name. With `type = "bytes"` this reads exactly
one byte; use `read_bytes` for anything longer.

### `write(address, type, value)` → boolean [, string]

Writes one value. All three arguments are required — unlike `read`, `type` has
no default. Returns `true`, or `false, message`.

Raises on an unrecognised type name, or if `value` is not convertible: an
integer for the `i*`/`u*` types, a number for `f32`/`f64`, a hex string for
`bytes`.

### `read_u32(address)` → integer | nil

Shorthand for `read(address, "u32")`. Returns `nil` with **no message** on
failure or a short read.

### `write_u32(address, value)` → boolean

Shorthand for `write(address, "u32", value)`. Returns a bare boolean; the
failure message is not available.

### `read_bytes(address, size)` → string | nil

Returns an uppercase, space-separated hex string (`"48 8B 05"`), or `nil` if the
read failed outright.

A **partial** read is not an error here: you may get back fewer than `size`
bytes, and an empty string is possible. Check the length yourself if it matters.

### `write_bytes(address, hex)` → boolean

Writes the parsed hex string. Returns a bare boolean, no message.

## Scanning

Scans run asynchronously on the scan job's thread. Starting a scan first cancels
*and joins* any scan already in flight, so `scan_exact`, `scan_unknown` and
`scan_next` can block briefly while the previous one winds down.

None of them require an attached process — an unattached scan simply finds
nothing.

### `scan_exact(value, type = "i32")`

Starts a first scan for an exact value. **Returns nothing**; poll
`scan_status()` or call `scan_wait()`.

Raises on an unrecognised type name, or if `value` does not convert to that type.

### `scan_unknown(type = "i32")`

Starts an unknown-initial scan, snapshotting every eligible location as a
baseline for a later `scan_next`. **Returns nothing.**

Raises on an unrecognised type name.

### `scan_next(mode [, value])` → boolean [, string]

Narrows the previous result set. `mode` is one of `"exact"`, `"unknown"`,
`"changed"`, `"unchanged"`, `"increased"`, `"decreased"`.

`value` is read **only** when `mode == "exact"`, and is interpreted using the
value type of the previous scan — you cannot change type partway through a chain.

Returns `true`, or `false, "There are no results to narrow. Run scan_exact or
scan_unknown first."` Raises on an unrecognised mode.

`"unknown"` as a next-scan mode matches everything: it re-reads the current
values without filtering, which is how you refresh a result set in place.

### `scan_status()` → table

Never fails. Returns:

| Field | Type | Meaning |
| --- | --- | --- |
| `running` | boolean | The scan thread is still working |
| `results` | integer | Results found so far |
| `fraction` | number | Progress, 0.0 to 1.0 |
| `truncated` | boolean | The result cap stopped the scan early |
| `status` | string | Human-readable status line |

`truncated` is distinct from cancelled: it means there were more matches than
the result limit allowed.

### `scan_wait(timeout_ms = 60000)` → boolean [, string]

Polls every 10 ms until the scan is idle. Returns `true` — immediately, if no
scan was ever started — or `false, "Timed out waiting for the scan."`

Ends the script if it is cancelled while waiting. That cannot be caught with
`pcall` — see [Cancellation](#cancellation).

### `scan_results(limit = 1000)` → table, integer

Returns **two** values: an array of at most `limit` results, and the total number
of results held. The total is how you detect that you are looking at a partial
view.

Each element:

| Field | Type | Meaning |
| --- | --- | --- |
| `address` | integer | |
| `value` | typed | Current bytes decoded with the scan's value type |
| `hex` | string | Current bytes as uppercase hex, **no separators** |

Note that `hex` here has no spaces, while `read_bytes` returns spaced hex. The
previous-scan bytes are not exposed.

## Pointer chains

### `resolve(module_name, module_offset, offsets)` → integer | nil, string

Walks a pointer chain and returns the address it currently points at. The module
is looked up by name at call time, which is what makes a chain survive a restart
under ASLR.

`offsets` must be a sequence (array-style table) of integers; a non-numeric entry
silently reads as 0. Each offset dereferences the current address and then adds
the offset, so the **final offset is added, not dereferenced** — the returned
address is where the value lives.

Returns the address, or `nil, message` when nothing is attached, the offsets
table is empty, the module is not loaded, or a pointer partway along is null or
unreadable.

```lua
-- helper.exe+0x3040 -> +0x10 -> +0x8
local addr, err = resolve("helper.exe", 0x3040, { 0x10, 0x8 })
if addr then print(string.format("%X = %d", addr, read(addr, "i32"))) else print(err) end
```

## Address list

### `add_address(address, type = "i32", description = "Lua entry", group = "Lua")` → integer

Adds an entry to the address list and returns its id. If a process is attached
and the type has a fixed size, the current value is captured as the entry's
freeze value.

Raises on an unrecognised type name.

## Code in the target

These three go through the injector and fail with `"No target process
attached."` when nothing is attached. All of them can destabilise or crash the
target — that is the nature of running code inside someone else's process.

### `alloc(size)` → integer | nil, string

Allocates `size` bytes in the target as **`PAGE_EXECUTE_READWRITE`**. This is not
configurable from Lua; if you want a different protection, use the Injection
panel.

Returns the base address, or `nil, message`. There is no `free` counterpart in
the Lua API — the allocation lives until the target exits.

### `thread(start, parameter = 0)` → boolean, integer | string

Creates a thread in the target at `start` and waits up to 5 seconds for it.
Returns `true, exit_code` or `false, message`.

A thread still running after 5 seconds returns `false` with a message saying so,
rather than reporting a bogus exit code. The thread keeps running.

### `loadlibrary(path)` → boolean, integer | string

Loads a DLL into the target via a remote `LoadLibraryW`. Returns
`true, base` or `false, message`.

The integer is the module's **base address**, looked up by filename after the
load. The module list is refreshed as part of this, so the newly loaded DLL is
also visible to `modules()` immediately afterwards.

If the load succeeds but the module cannot then be found in the list, the remote
thread's exit code is returned instead — the low 32 bits of the module handle,
which is not a usable address. Compare against `modules()` if you need to be
certain.

Fails with an explicit message on a 32-bit (WOW64) target.

## Sandbox

These are removed before your script runs:

- **Globals:** `io`, `package`, `require`, `dofile`, `loadfile`
- **From `os`:** `execute`, `remove`, `rename`, `tmpname`, `exit`, `getenv`,
  `setlocale`

Everything else from the standard library remains, including `os.time`,
`os.clock`, `os.date`, `os.difftime`, `load`, `string`, `table`, `math`,
`coroutine` and `debug`.

This is a guard against a careless script, not a security boundary. `load` is
still present, and a memory-editing tool hands you the ability to write
arbitrary bytes into another process regardless. Do not run a script you have
not read.

## Cancellation

Your script runs on its own coroutine. Stop sets a flag that three things watch:
a VM hook that fires every 10 000 instructions, `scan_wait`'s 10 ms poll, and
`check_cancel`. Any of them **yields**, which ends the script — Pointer Lab
simply never resumes the coroutine, and prints `Script cancelled.`

**A cancel cannot be caught.** `pcall` and `xpcall` catch errors; a yield passes
straight through them. `while true do pcall(f) end` stops when you press Stop.

Stop also cancels a scan the script started, so the work stops with the script
rather than continuing in the background.

Still worth knowing:

- **Time inside a C function is not interruptible.** A long `read_bytes`, or the
  join at the start of a scan, runs to completion. Only Lua instructions,
  `scan_wait` and `check_cancel` observe the flag.
- The Lua state is shared between runs, so globals set by a cancelled script are
  still there on the next one.

### `cancelled()` → boolean

Whether Stop has been pressed. Useful inside a loop that spends its time in C
functions, where the VM hook rarely gets a turn:

```lua
for i = 1, 100000 do
    if cancelled() then print("stopping early") break end
    read_bytes(base + i * 16, 16)
end
```

### `check_cancel()`

Ends the script immediately if Stop has been pressed, and does nothing
otherwise. Like the hook, this cannot be caught with `pcall`.

## Example

```lua
-- Find a known i32, narrow it after it changes, and freeze what is left.
if not attach(4812) then print("could not attach") return end

scan_exact(100, "i32")
scan_wait()
print(scan_status().results .. " candidates")

print("change the value in the target, then run the rest")

scan_next("decreased")
scan_wait()

local results, total = scan_results(10)
print(total .. " remain; first " .. #results .. ":")
for _, r in ipairs(results) do
    print(string.format("  %X = %s", r.address, tostring(r.value)))
end

if total == 1 then
    add_address(results[1].address, "i32", "Found by script", "Lua")
end
```
