# MCP API reference

Pointer Lab can expose its engines over the [Model Context
Protocol](https://modelcontextprotocol.io), so an AI agent can drive a session:
attach to a process, scan, read and write memory, follow pointer chains, patch
code. Everything here is implemented under
[`src/mcp/`](../src/mcp/) and driven from the **MCP Server** panel.

The server is **embedded in the running application**, not a separate tool. The
process it attaches to is the one on screen, a scan it starts fills the Scanner
panel, and an address it adds appears in the address list a frame later. That is
the point of it: an agent and a person work on one session rather than two.

## Read this first

**Starting the server is the only thing you will be asked.**

Everywhere else in Pointer Lab, anything that allocates in, injects into,
patches or detaches from a live process asks first. The MCP server does not. It
is off when the application starts, and while it is running a client holding the
token can read and write the target's memory, patch its code, set breakpoints,
allocate, load a DLL into it and start threads in it, with no further
confirmation.

So the decision is made once, when you press Start, and it covers everything
that follows. Two things follow from that:

- **Stop the server when you are done.** It is not stopped by detaching, by
  loading a project, or by the target exiting.
- **Nothing an agent does here is undone when it disconnects.** Patches stay
  applied, allocations stay allocated, loaded libraries stay loaded, and frozen
  values stay frozen. The Patches panel and `patch_restore_all` are the undo;
  there is no undo for the injection tools at all.

The transport is bound to `127.0.0.1` and requires a bearer token, so another
machine cannot reach it and another local program cannot use it without the
token. That is access control, not a safety check: it decides *who* may call,
not *what* they may do. Anything holding the token has the same reach over the
target that you do.

## Connecting

Open **View → MCP Server**, press **Start server**, and press **Copy claude mcp
add command**. That puts a complete registration command on the clipboard:

```
claude mcp add --transport http pointerlab http://127.0.0.1:8722 --header "Authorization: Bearer <token>"
```

The token is regenerated every time the server starts, is never written to disk,
and does not survive a restart — so the command has to be re-copied after each
start.

`PointerLab.exe --mcp [port]` starts the server as the window comes up and opens
the panel, for someone who runs Pointer Lab in order to hand it to an agent. The
port defaults to 8722; `0` lets Windows choose a free one. The flag saves a
click, not the decision — the token still has to be read off the panel.

## Conventions

**Addresses** can be given as a number or as any expression the address boxes in
the window accept: `"client.dll+0x4A2C10"`, `"kernel32.LoadLibraryW"`,
`"game.exe"`, a symbol defined with `symbol_define`, or bare hex. Offsets inside
an expression are **always hexadecimal**. Prefer an expression: it still names
the right thing after the target restarts and ASLR moves the module, and a bare
address does not.

Addresses come back **twice** — `address` as a number to feed to the next call,
and `hex` as a string to show a person.

**Value types**, case-insensitive: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`,
`i64`, `u64`, `f32`, `f64`, `bytes`, `str`, `wstr`. The same names the Lua API
uses.

**Scan modes** are the names the Scanner panel shows — `"Exact value"`,
`"Increased by"`, `"Same as first scan"` — matched without regard to case,
spaces, hyphens or underscores, so `"increasedby"` works too.

**Hex input** is parsed leniently: every non-hex character is discarded and an
odd number of digits is left-padded with `0`, so `"48 8B 05"`, `"488b05"` and
`"48-8B-05"` are one input. Byte *patterns* additionally accept `?` and `??` as
wildcards.

**Errors** come back as a normal tool result with `isError` set and a sentence
saying what went wrong, not as a JSON-RPC error — a protocol error is the
client's bug and is not shown to the model, whereas these are meant to be read
and acted on. Where Windows supplied a code it is appended, because "needs
elevation" and "that page is not mapped" are different problems.

Arguments that can be checked without a target are checked first, so a wrong
value type is reported as a wrong value type rather than as "attach first".

## Tools

Names are shared with the Lua API wherever the two do the same job; those names
are a published contract for the life of 3.x.

### Target

| Tool | What it does |
| --- | --- |
| `processes` | Every running process, as pid and name. Works with nothing attached — it is how you find the pid. |
| `attach` | Attach by `pid`. Everything else needs this first. Reports `read_only` when only a read-only handle could be opened, which means every write, freeze and patch will fail. |
| `detach` | Always succeeds, including when nothing is attached. |
| `refresh` | Re-read the module and region lists. Needed to see a module loaded *after* you attached. |
| `session_info` | What is attached: pid, name, pointer width, `read_only`, `exited`. |
| `modules` | Loaded modules, with an optional `filter` substring. |
| `regions` | Committed regions and their protection, with `writable_only` / `executable_only` / `limit`. |

### Memory

| Tool | What it does |
| --- | --- |
| `read` | One typed value. A **short read is an error** here rather than a half-decoded value. |
| `write` | One typed value. Changes a live process. |
| `read_bytes` | Raw bytes as hex, up to 4096. A **partial read is not an error** — compare `length` against `requested`. |
| `write_bytes` | Raw bytes from hex. Changes a live process. |
| `read_pointer` | One pointer at the target's own width — four bytes on a 32-bit target, eight on a 64-bit one. |

### Scanning

Scans run in the background. `scan_first` and `scan_next` return as soon as the
scan has started; poll `scan_status` until `running` is false, then read
`scan_results`.

| Tool | What it does |
| --- | --- |
| `scan_first` | Start a first scan. A mode that compares against an earlier scan records a baseline instead of filtering — `baseline_only` says so. |
| `scan_next` | Narrow the previous results. The value type is the first scan's and cannot change partway through a chain. |
| `scan_status` | `running`, `fraction`, `results`, `status`, `truncated`. `truncated` means the result cap stopped the sweep early, which is not the same as cancelled. |
| `scan_results` | A page of results with `offset` / `limit` (capped at 1000), plus `total` so a partial view is recognisable. Each row carries `static`, which is true for an address inside a loaded module — the ones worth writing down. |
| `scan_cancel` | Stop the scan in flight. |
| `scan_set_options` | `max_results`, `writable_only`, `executable_only`, `float_epsilon`. Persist until changed. |
| `find_pattern` | Synchronous byte-pattern search over a range. Returns *every* match up to the cap, because a pattern that matches twice is not specific enough to rely on. |

### Symbols

| Tool | What it does |
| --- | --- |
| `resolve` | An expression to an address. |
| `describe` | An address to the most specific name for it, or nothing. Never invents a name for a heap address. |
| `symbol_define` | Name an address or an expression. The **expression** is stored, so the symbol survives a restart. |
| `symbol_undefine`, `symbols_list` | Forget one; list them all. |

### Pointer chains

Each offset dereferences the current address and then adds the offset, so the
**final offset is added rather than dereferenced** — the address returned is
where the value lives.

| Tool | What it does |
| --- | --- |
| `resolve_chain` | Walk `module` + `module_offset` + `offsets` and return where it points now. |
| `pointer_scan_start` | Find chains leading to an address. Background; poll `pointer_scan_status`. |
| `pointer_scan_filter` | Keep only the chains that still resolve to a new address. This is the step that separates a real chain from a coincidence, and it costs a few reads per chain rather than another sweep. |
| `pointer_scan_status`, `pointer_scan_results` | Progress, and a page of chains. |

### Address list

| Tool | What it does |
| --- | --- |
| `add_address` | Track a fixed address. |
| `add_chain_address` | Track a pointer chain, so the entry keeps pointing at the value across a restart. |
| `list_addresses` | Every entry with its current value, group, freeze state and chain. |
| `remove_address` | Remove one by id. |
| `set_frozen` | Freeze or release. A frozen entry is written back twenty times a second, which is what holds a value against a program trying to change it. |
| `update_value` | Write a new value in the entry's own type. |

### Structures

| Tool | What it does |
| --- | --- |
| `struct_add`, `struct_list` | Create a layout; list them with their fields. |
| `struct_set_fields` | Replace the fields. Each is `{offset, type, name, length}`; `length` applies only to `bytes`/`str`/`wstr`, whose width does not come from the type. Overlapping fields are refused. |
| `struct_read` | Read the layout at up to 8 addresses at once. Each row carries `identical` — whether every address holds the same bytes there. That comparison is the whole point: the fields that differ between two objects are the ones that describe them. |
| `struct_guess` | Fill a structure in from what is actually there, typed by what the bytes look like. A guess, and labelled as one. |

### Code

| Tool | What it does |
| --- | --- |
| `disassemble` | Decode instructions. Bytes that do not decode come back with `valid` false rather than being skipped, so the listing stays aligned. |
| `assemble` | Intel syntax to bytes, **at a given address** — a relative jmp or call encodes differently depending on where the code will live. Uses the target's bitness. |
| `alloc` | Allocate in the target, optionally `near` an address so a five-byte jmp can reach it. Not freed on disconnect. |
| `free` | Release a block. Freeing memory the target is still using will crash it. |
| `create_thread` | Start a thread and wait up to five seconds. A thread still running after that is reported as such rather than given a bogus exit code, and it keeps running. |
| `load_library` | Remote `LoadLibraryW`. Stays loaded until the target exits. |
| `aa_check` | Compile an auto-assembler script **without writing or allocating anything**, and report what it would do. This is how a script is read critically before it runs. |
| `aa_add`, `aa_set_enabled`, `aa_disable_all`, `aa_scripts` | Manage scripts. `aa_set_enabled` returns the run's notes, which say what the script actually resolved — far more useful than "it worked". |

### Breakpoints and the access watch

| Tool | What it does |
| --- | --- |
| `debugger_attach`, `debugger_detach` | Needed before any breakpoint can fire. |
| `breakpoint_add` | `software` replaces a byte with int3 and there can be any number. `execute`/`write`/`readwrite` use a debug register instead — nothing in the target is modified and they can watch data, but there are exactly four. |
| `breakpoint_remove`, `breakpoints_list` | Remove one; list them with hit counts and the last thread's registers. |
| `breakpoint_events` | Hit messages queued since the last call. Rate limited; the hit counts are the authoritative record. |
| `access_watch_start` | "What writes to this address." Aggregates every hit by the instruction responsible. |
| `access_watch_stop`, `access_watch_sites` | Stop; read the instructions found. Each site carries the interpreted registers — `"watched address is RDI+0xF8"` says the structure's base is in RDI, which is the step from one address to every instance of the same object. `instruction_resolved` is false when the walk back failed, in which case the address is the trap address. |

### Patches

| Tool | What it does |
| --- | --- |
| `patch_apply` | Overwrite code, recording what was there. Padded with nops to cover whole instructions by default, because a patch shorter than what it replaces leaves a truncated instruction that crashes the target. |
| `patch_set_enabled`, `patch_remove`, `patch_restore_all` | Apply, restore, forget. |
| `patches_list` | Every patch, and `drifted` — whether the bytes there still match what was written, which is how you notice the target rewriting its own code. |

### Speed

`speed_load`, `speed_set_scale` (0.05 to 20), `speed_reset`, `speed_status`.
`hooked_imports` being zero after a successful injection is the honest and
important case: that program does not ask for the time through its import table.

### Project and window

`project_save` and `project_load` read and write `.iretable` files.
`screenshot`, `select_panel`, `set_layout`, `set_window_size` and `quit` drive
the window — `select_panel` is worth using, because it puts the person in front
of what you are working on. All of these need a window and report
`"There is no window to drive."` without one.

## What is not here

- **No event stream.** Every tool is request/response; the `GET` half of
  Streamable HTTP is refused rather than held open for events that never come.
  Poll `scan_status`, `breakpoint_events` and `access_watch_sites`.
- **No progress notifications**, for the same reason.
- **No sandbox.** There is nothing to sandbox: the tools are the tool's own
  features, and a memory editor's features are the ability to change another
  process however you like.

## Example

Attach to the tutorial, find a value, and track it — the shape of nearly every
session.

```
processes                                   -> find PointerLabTutorial.exe, note the pid
attach            { pid }
scan_set_options  { writable_only: true }
scan_first        { mode: "Exact value", type: "i32", value: 100 }
scan_status                                  -> poll until running is false
scan_results      { limit: 10 }              -> 42 results
                                             -- change the value in the target
scan_next         { mode: "Decreased value" }
scan_status                                  -> poll
scan_results      { limit: 10 }              -> one left
add_address       { address, type: "i32", description: "health" }
set_frozen        { id, frozen: true }
select_panel      { name: "Address List" }   -- so the person sees what you found
```
