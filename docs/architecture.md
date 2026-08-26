# Architecture

Pointer Lab is a single executable built from one static library
(`pointerlab_core`) plus a thin UI/entry-point layer. The core library holds
everything that can be tested without a window, which is why the test suite can
exercise the scanner, the debugger and the crash handler directly.

## Layers

Dependencies point downwards only. Nothing in a lower layer knows about a
higher one.

```
        main.cpp  ──►  app/Application  ──►  ui/UiApp
                                                │
                                                ▼
                                    services/RuntimeServices
                                                │
        ┌───────────────┬───────────────┬───────┴───────┬──────────────┐
        ▼               ▼               ▼               ▼              ▼
  engine_scan     engine_pointer   engine_disasm    engine_asm    engine_inject
  engine_debug    engine_patch     engine_symbols   engine_aa     engine_struct
  engine_speed    engine_export
        └───────────────┴───────┬───────┴───────────────┴──────────────┘
                                ▼
                       domain/TargetSession
                                │
                                ▼
                    platform_win32/Win32Platform
                                │
                                ▼
                      domain/  ·  infra/
```

`scripting/` sits alongside the engines: it drives them, and is driven by the UI.
`storage/` depends only on `domain` and `infra`.

`engine_aa` is the one engine that borrows others — the assembler, the patch
registry, the symbol table and the injector — because an auto-assembler script is
by definition all four at once. `engine_speed` borrows the injector and the export
resolver for the same reason. Both still point only downwards: they know nothing
of `services/` or `ui/`, and `RuntimeServices` declares them after everything whose
references they hold.

`src/payload/` is not a layer at all. It is a separate DLL, compiled into the
target's address space rather than ours, and it shares no header with the rest of
the tree — the only contract between the two sides is five exported variables
looked up by name.

### `infra/` — no domain knowledge

- **`Result<T>`** is the error type used everywhere below the UI. The class is
  `[[nodiscard]]`, so a dropped result is a compile error under `/WX`. It carries
  a message and an optional platform error code, which is what lets a caller tell
  `ERROR_ACCESS_DENIED` apart from `ERROR_PARTIAL_COPY`.
- **`Logger`** is a process-wide singleton with a level filter, a bounded
  in-memory ring for the Logs panel, and a file held open for the process
  lifetime. Every line is flushed, because the last line before a crash is the
  one that matters.
- **`CrashHandler`** installs the SEH filter plus the `terminate`,
  invalid-parameter and pure-call handlers, so an uncaught C++ throw produces a
  minidump as reliably as an access violation. Everything it needs is
  pre-allocated at install time and it writes with raw Win32 calls — a faulted
  process may have a corrupt heap, and the Logger's mutex may be held by the
  thread that just died.
- **`Paths`** resolves the `%LOCALAPPDATA%\PointerLab` locations.

### `domain/` — types and pure functions

Plain data (`AddressEntry`, `PointerChain`, `MemoryRegion`, `ModuleInfo`,
`ScanValue`, `Instruction`, `BreakpointInfo`, `RegisterContext`) plus the
parsing and formatting helpers around them. Deliberately free of Windows types:
`RegisterContext` holds seventeen named integers rather than a `CONTEXT`, so the
UI, the tests and the storage layer never include `Windows.h`.

`TargetSession` is the exception — it is the one stateful object here, owning the
handle to the attached process and forwarding reads and writes to the platform
layer. Everything above it takes a `TargetSession&` and therefore does not care
how the process is reached.

### `platform_win32/` — every Win32 call

`Win32Platform` is the only place that talks to the operating system: process
and module enumeration, `ReadProcessMemory`/`WriteProcessMemory`, region walking,
privilege acquisition, and the debug event pump.

**`DebugEventPump`** deserves its own note. `DebugActiveProcess`,
`WaitForDebugEvent`, `ContinueDebugEvent` and `DebugActiveProcessStop` must all
run on the same thread, so the pump owns a dedicated thread and the public API
(`attach`, `addBreakpoint`, `removeBreakpoint`) marshals onto it. A breakpoint
hit runs the full cycle: rewind RIP to the trap, restore the original byte, set
the trap flag, single-step, re-arm the `0xCC`. Detaching drains every queued
event first and recognises a trap that was already in flight when the breakpoint
was removed, because delivering a stale `int3` to a process that no longer has a
debugger kills it.

Hardware breakpoints share the table and none of that cycle. They live in the
CPU's four debug registers (`DR0`–`DR3`, controlled by `DR7`), so nothing in the
target is modified and nothing is ever disarmed — the window in which a software
breakpoint can be missed does not exist for them. The registers are per-thread,
so the pump programs every thread the target has and every thread it later
creates, always writing them wholesale from the breakpoint table rather than
patching bits, so a thread created mid-add cannot disagree about `DR7`. A hit
arrives as `EXCEPTION_SINGLE_STEP` with `DR6` naming the register; an execute
breakpoint faults *before* its instruction, so `EFLAGS.RF` is set on resume or
the thread would fault on it forever. The stale-trap problem has a hardware twin:
a trap already in flight when its register is cleared arrives with `DR6` wiped
and nothing to match it against, so once debug registers have been used in an
attach, an unclaimed single-step exception is treated as ours. Passing it on
would be a certain kill; swallowing somebody else's costs nothing.

### Target bitness

`TargetSession` settles the target's pointer width once, at attach, from
`IsWow64Process`, and everything above it asks rather than assuming. This matters
in more places than it first appears, and every one of them failed *silently*
before:

- `engine_pointer` strides and reads 4-byte pointers for a 32-bit target. Reading
  8 finds nothing at all — the scan completes normally and reports no chains.
- `engine_disasm` and `engine_asm` select the Zydis machine mode and the Keystone
  mode to match. Most byte sequences decode as *something* in both modes, so the
  wrong one produces a plausible, wrong listing rather than an error; and `inc
  eax` assembles to one byte in 32-bit and two in 64-bit, because `0x40` became
  the REX prefix.
- `DebugEventPump` reads and writes the 32-bit thread context through
  `Wow64Get/SetThreadContext`. A WOW64 thread has two contexts, and the 64-bit
  one that `GetThreadContext` returns describes Windows' emulation layer in
  `wow64cpu.dll`, not the target's code. Using it is not an error the API
  reports — it is a valid context belonging to the wrong layer.
- `engine_symbols` resolves `LoadLibraryW` out of the target's own kernel32 for
  injection, because a 32-bit process has an entirely different one mapped.

`domain::RegisterContext` carries the bitness it was captured with, so the
breakpoint panel names registers `EAX`/`EIP` and hides `r8`–`r15`, which do not
exist in a 32-bit thread.

### `engine_debug/` — the answer, not just the mechanism

`AccessWatch` sits behind "find out what writes to this address". The mechanism
is a hardware data breakpoint the debug pump already provided; this is the layer
that makes it an answer rather than a firehose.

Three problems it solves, none of which is the breakpoint itself:

- **Aggregation.** Hits are grouped by the instruction responsible and sorted
  busiest first. Resolution is cached per instruction, so a watch inside a render
  loop resolves two or three sites and then costs a map lookup per hit. The
  resolve itself runs *outside* the mutex, because it reads and disassembles
  target memory and holding the lock across that would stall the UI thread
  polling `sites()`.
- **The off-by-one instruction.** A data breakpoint traps *after* the access, so
  the reported instruction pointer names the instruction after the one that
  touched the address. `engine_disasm::precedingInstruction` walks back — see
  below — and when it cannot, the site says so and the NOP action is disabled.
  An execute breakpoint faults *before* its instruction and needs none of this,
  which is why the kind decides the path.
- **Interpretation.** `explain()` describes each captured register against the
  live target: inside a module (`module+offset`, i.e. static), inside mapped
  memory, or a short way below the watched address — in which case it is the
  probable base of the structure containing the value. That last case is the
  point of the whole feature.

**Backward disassembly** deserves a note, because it is not decidable. x86
instructions are 1–15 bytes with no alignment, so nothing in the bytes says
where the previous one began. What makes it tractable is that x86 is
self-synchronising: decoders started at different offsets converge on the same
boundaries within a few instructions. `precedingInstruction` therefore decodes
from every start position within 24 bytes, has each vote for the instruction it
finds ending at the target, and takes the boundary with the most votes. Trusting
a single long lookback is not enough — the bytes before a function need not be
code, and a run of them can decode cleanly and land on the target by luck. That
is not hypothetical; it is what the first implementation did, and a test caught
it naming a two-byte fragment of a `mov`'s immediate operand.

### `engine_patch/` — patching that can be taken back

`PatchRegistry` owns every byte Pointer Lab writes into a target's code, paired
with the bytes that were there first. Anything that modifies code goes through
it — the assembler patch box and the Access Watch's nop action both do — so the
Patches panel is a complete account rather than a partial one.

The invariant it exists to protect is that every recorded "original" is
genuinely original. Three consequences:

- **Overlapping ranges are refused.** A patch landing inside an existing one
  would capture that patch's *replacement* bytes as its original, and disabling
  the pair in the wrong order would leave a mixture of the two that was never
  real code. There is no ordering rule that fixes this, so the write is refused.
- **The original is read before the patch is written, and a short read is a
  failure.** Recording four bytes of a six-byte original and calling it a
  success would mean the undo restores a truncated instruction.
- **`forgetAll` does not touch the target, and is a separate operation from
  `restoreAll`.** Detaching uses the former: a patch left applied may be the
  point of the session, and Pointer Lab has no standing to revert it. What the
  UI owes the user there is the sentence saying it is about to become permanent.

`drifted()` compares what is at the address against what the registry last wrote
there. Divergence means something else changed the code — the target rewriting
itself, an anti-tamper check restoring it, a write that bypassed the registry —
and the panel flags it, because a tick box asserting a state the memory does not
have is worse than no tick box.

This is also the substrate an `[ENABLE]`/`[DISABLE]` script model needs: a
script that installs a hook is a set of patches, and disabling it is restoring
them.

### `engine_symbols/` — exports without a symbol server

`ExportResolver` parses a module's PE export directory out of the *target's*
memory. `GetProcAddress` answers about Pointer Lab's own address space, which is
the wrong answer twice over: a WOW64 target has a different kernel32 entirely,
and even a same-bitness target need not have it at the same base.

`SymbolTable` sits on top of it and answers in both directions.

Forwards, it parses the expressions every address box in the tool accepts:
`client.dll+0x4A2C10`, `kernel32.LoadLibraryW+0x10`, a module name alone, a user
symbol, or a plain hexadecimal address, each followed by any number of `+`/`-`
terms. Two details are load-bearing:

- **Offsets are hexadecimal, always.** Inferring the base from the digits
  present would make `+00400000` decimal and resolve, silently, somewhere else
  entirely. The same rule already applies to `domain::parseAddress`.
- **`module.export` splits at the *last* separator.** `kernel32.dll.LoadLibraryW`
  has to become `kernel32.dll` and `LoadLibraryW`, not `kernel32` and
  `dll.LoadLibraryW`. `!` is accepted as well, because debugger notation is what
  half the world writes.

A user symbol is stored with the expression that produced it, not only the
address that expression yielded, and the project file saves the expression. An
address recorded in one run names nothing in the next; `client.dll+0x4A2C10`
names the same thing every run, which is the entire reason to write it down.

Backwards, `describe()` gives the most specific name for an address — a user
symbol that names it exactly, else `module.dll+0x1234`, else nothing. It never
invents a name for a heap address. `isStatic()` is the same question reduced to
a yes/no, and it is what the scan results and address list colour green: an
address inside a module image is at the same offset every run, and that is the
difference between an address worth recording and one that is wherever the
allocator put it today.

It follows forwarder chains, which is not optional — `kernel32!LoadLibraryW` is a
forwarder to `KERNELBASE` on every modern Windows, and an RVA landing inside the
export directory is a `"DLL.Function"` string rather than code. A resolver that
missed that would hand a remote thread an address inside a string table.

Parsing the headers directly also means no DbgHelp, no PDBs and no symbol
server: an export directory is present in every PE by construction.

### `engine_aa/` — an injection written down rather than performed

`AutoAssembler` interprets a script with an `[ENABLE]` section that patches the
target and a `[DISABLE]` section that puts it back. The point of the format is
that a code injection becomes a *description* — "find these bytes, allocate a
cave within reach of them, write this code into it, and jump to it from there" —
which can be read, checked, kept and re-run after the target restarts. None of
that is true of the same work done by hand in a hex editor.

It sits on top of `engine_asm` (Keystone), `engine_patch` (the undo), `engine_scan`
(`findPattern`) and `engine_symbols` (address expressions), and adds four things
of its own:

- **Compile is separable from run.** `check()` does everything except write:
  directives are executed, patterns are scanned for, asserts are evaluated, the
  layout is computed against plausible addresses, and the result says what
  *would* be written and where. Reading a script critically before running it is
  only possible if you can see what it worked out.
- **Layout is iterated to a fixed point.** An instruction's length can depend on
  a label's value — a jump somewhere close encodes in two bytes and somewhere far
  in five — and a label's value depends on the lengths of everything before it.
  So the layout is recomputed until it stops moving, capped at eight passes.
  Undefined labels start half a gigabyte above the lowest address the script
  already knows, which is far enough to force the long encoding and near enough
  to *have* one: a fixed low placeholder cannot be reached by a rel32 from a
  module at `0x7FF7194C0000`, so the first pass would fail to encode a script
  that is perfectly fine.
- **`aobscanmodule` refuses a pattern that matches twice.** Taking the first
  match produces a script that works today and patches something unrelated after
  the next update. A pattern that matches twice is a pattern that is not specific
  enough, and saying so is the only answer that stays true later.
- **A failed run rolls back.** Patches applied before the failure are removed in
  reverse and allocations are freed, so a script never leaves the target half
  injected. `[DISABLE]` restores the registry's patches *first* and frees the
  cave second — a thread already on its way into a cave that has been unmapped
  lands in nothing.

`alloc(name, size, near)` uses `Win32Platform::allocateNear`, which walks free
regions with `VirtualQueryEx` inside ±2 GB of the hint and **fails** rather than
falling back to a distant allocation. A five-byte `jmp` encodes a signed 32-bit
displacement; a cave out of that range cannot be jumped to at all, and a
successful allocation there would only move the failure somewhere less obvious.

Sections may begin at any address expression, not only at a name the script
bound: `7FF612340000:` and `game.exe+8A3F1:` both work, which is what
`define(INJECT, …)` followed by `INJECT:` turns into.

### `engine_struct/` — one layout over several objects

`Dissector` holds named layouts — an ordered, non-overlapping list of
`(offset, type, name)` — and lays one over several addresses at once.

The comparison is the feature, not the display. A game does not have one player,
it has an array of them, and the fields that differ between two of them are the
ones that describe a player; the fields that are the same are padding or shared
state. So `read()` returns a per-row `identical` flag computed across the
addresses that could be read — an instance that has gone away must not make the
remaining ones look like they disagree — and the panel greys those rows rather
than hiding them.

Four decisions worth naming:

- **One read per address, not one per field.** A forty-field structure over four
  instances is 160 cross-process round trips the other way, and this runs every
  frame.
- **Overlapping fields are refused**, for the same reason overlapping patches
  are: the display would have to pick which of the two owns a byte, and whichever
  it picked would be wrong half the time.
- **Negative offsets are legal.** Where an object starts is a guess, and
  discovering the real one begins earlier must not mean renumbering everything
  below it. The window read therefore starts at the lowest offset used, not at
  zero.
- **The auto-guess is a guess and says so.** A slot is called a pointer when its
  value at every address is either null or inside a committed region, and at
  least one is non-null — otherwise every run of zeroes in an object would be a
  pointer. A slot is called a float when its bytes decode to a finite,
  non-denormal value between 1e-6 and 1e9. That heuristic works because the two
  interpretations barely overlap: a small integer reads as a denormal around
  1e-43, and an ordinary float reads as an integer in the hundreds of millions.
  Eight-byte slots are only considered at eight-byte alignment, because a
  compiler does not put a pointer where it is not aligned and looking anyway
  finds "pointers" straddling two unrelated fields.

Only the definitions are persisted. A layout is knowledge about the game and
outlives every run; the address of one particular enemy does not survive the next
respawn.

### `engine_speed/` — a hook on the clock, not on the game

`SpeedController` is the near side of the speed hack; `src/payload/SpeedHook.cpp`
is the far side, built twice (`PointerLabSpeed64.dll` and `PointerLabSpeed32.dll`)
because a DLL has to match the process it is loaded into.

A game does not measure time. It asks Windows what time it is, works out how much
has passed since it last asked, and multiplies everything it does that frame by
that delta. So there is nothing game-specific to find: changing the *rate* of the
four clocks it can ask changes the rate of the game, and the hook needs to know
nothing about it.

Four decisions worth naming:

- **The clock is rebased, not multiplied.** `return real * scale` jumps by hours
  the instant the scale changes and jumps *backwards* when it is lowered again. A
  game that sees time go backwards does not slow down; it divides by a negative
  delta and detonates. Each clock instead keeps the real and fake values from
  when the rate last changed and answers
  `fake = baseFake + (real - baseReal) * scale`, re-reading both bases on every
  change. The reported clock is continuous and monotonic across every change,
  which is the property the game depends on.
- **Imports are patched, not functions.** An inline hook means decoding the first
  instructions of the real function to know how many to copy, allocating a
  trampoline, and getting all of it right while other threads execute the bytes
  being rewritten. Every `call [__imp_QueryPerformanceCounter]` instead reads its
  destination from a table in the game's own image; writing a different address
  there redirects all of them at once, and the undo is writing the old value back.
  What it misses is a call that does not go through the table — an address from
  `GetProcAddress`, or a call from inside a system DLL — and that is reported
  (`hookedImports`) rather than hidden.
- **The modules that implement the clocks are never patched.** This is a
  correctness requirement, not caution. kernel32 is largely stubs that reach the
  implementation in kernelbase *through kernel32's own import table*, so patching
  it sends the hook's call to the real function straight back into the hook and
  the target dies of a stack overflow on the first frame that asks the time.
  `isClockModule()` skips ntdll, kernel32, kernelbase, winmm, winmmbase and the
  `api-ms-`/`ext-ms-` shims, and the real functions are resolved from kernelbase
  first so the downstream call does not depend on that list being complete.
- **Everything happens on a worker thread, not in `DllMain`.** Taking a module
  snapshot, calling `VirtualProtect` and creating a thread are all forbidden
  under the loader lock. `DllMain` therefore only stores the module handle,
  initialises a lock and starts the thread; `pl_alive` is what tells the near
  side the worker actually ran, which is a different question from whether the
  exports resolved.

The control block is five exported *variables* rather than functions, because a
remote thread can pass exactly one pointer-sized argument and a scale is a
double. `SpeedController` finds them by name with `engine_symbols::ExportResolver`
— the same parser injection uses — and reads and writes them across the process
boundary. `reset()` restores every import and stops the worker but leaves the DLL
loaded: unloading a module while a thread might be executing inside it is a crash
with no way to prove it will not happen.

`setScale` refuses anything outside 0.05x–20x rather than clamping, because a
silently clamped request for 1000x looks exactly like a hook that is installed
and doing nothing.

### `engine_export/` — a trainer as source, not as an executable

`TrainerExport` turns the address list into a small CMake project — `main.cpp`,
`CMakeLists.txt`, `README.md` — and deliberately not into an `.exe`.

The reason is pedagogical first. A generated binary is a black box: it works, and
the person holding it has learned nothing about why. Generated source is the same
trainer with its reasoning visible — here is how a process is found by name, here
is how a module base is looked up, here is the loop that walks a pointer chain one
dereference at a time. The practical reason is second and still good: nobody
should run an executable produced by somebody else's tool from somebody else's
table against their own machine, and source can be read first.

Generation is pure — options in, three strings out — so it is tested without
touching the disk. `write()` is separate and refuses to overwrite: the likeliest
reason for a collision is a trainer the user has since edited by hand. It checks
every file before writing any, so a collision on the third does not leave the
first two behind.

Entries are split rather than filtered. An entry with a frozen value becomes a
cheat; an entry without one has no value to write and is listed in the README
under "Not exported", because an entry that vanished from the export with no
explanation reads as a bug in the generator. Module-rooted entries emit
`module+offset` plus their offset chain; absolute ones emit the address and are
labelled, in the source and again in the README, as valid only for the run they
were found in.

### `engine_*/` — one job each

Each engine takes a `TargetSession&` and does one thing. The long-running ones
(`MemoryScanner`, `PointerScanner`) are job objects: `start()` spawns a worker,
`progress()` is safe to poll from the UI thread every frame, and `cancel()`
joins. `Disassembler` and `Assembler` are stateless and wrap Zydis and Keystone
respectively.

`padToInstructionBoundary()` lives in `engine_disasm` rather than in the UI on
purpose — deciding how many bytes a patch must cover is a disassembly question,
and putting it in the UI made it untestable.

### `services/RuntimeServices` — the composition root

Owns the session, the platform, every engine, the address list and the
breakpoint service, and runs the background loop that applies frozen values and
re-resolves pointer chains (about twice a second). This is the only object the
UI holds.

`BreakpointService` is a delegate over `DebugEventPump`: it converts hit
callbacks arriving on the pump thread into a queue the UI drains each frame, rate
limited to one notification per 500 ms so a breakpoint in a hot loop cannot
flood the interface. The hit count is not rate limited.

### `ui/UiApp` — Dear ImGui over DX11

One class, panel-per-method, across several translation units grouped by area. It
owns the Win32 window, the D3D11 device and the ImGui context, and holds
`RuntimeServices` by reference.

| File | Holds |
| --- | --- |
| `UiApp.cpp` | Window and device lifecycle, `render()`, toasts, confirmations, hotkeys |
| `UiLayout.cpp` | Visual style and the default docking layout |
| `UiMenu.cpp` | Menu bar and command bar |
| `UiPanelsTarget.cpp` | Process, modules, memory regions |
| `UiPanelsScan.cpp` | Scanner and address list |
| `UiPanelsMemory.cpp` | Hex viewer, disassembly, breakpoints |
| `UiPanelsTools.cpp` | Pointer scanner, injection, Lua scanner and console, log |
| `UiProject.cpp` | Project files, session autosave, settings |
| `UiWindows.cpp` | About and Help |

`UiInternal.h` carries the small helpers the panels share — `denseTableFlags`,
`helpMarker`, `statusPill`, `valueTypeNames` and the like — which used to sit in
an anonymous namespace at the top of the single file. They are `inline` in a
named namespace rather than in an anonymous one, so a file that does not happen
to use one does not trip `/W4 /WX` over an unused static function.

Two conventions matter here:

- Every user-visible message goes through `notifyInfo`/`notifyError`, which log
  and raise a toast. There is no path that reports a failure without recording it.
- Every destructive or irreversible action goes through `pendingConfirm_`, a
  single modal slot holding a title, a message and the action to run on confirm.

## Threading

| Thread | Owns |
| --- | --- |
| UI | The window, the ImGui context, the D3D11 device, all UI state |
| Freeze loop | Frozen-value writes and pointer-chain re-resolution |
| Scan worker | One `ScanJob` run |
| Pointer scan worker | One `PointerScanJob` run |
| Debug pump | Every debug API call, for the pump's whole lifetime |
| Lua worker | One script execution |

Everything crossing a thread boundary does so through a mutex-guarded snapshot
(`progress()`, `results()`, `snapshot()`, `takeEvents()`, `takeOutput()`) rather
than by sharing a live structure. The UI never blocks on a worker.

## Dependencies

All fetched and built by CMake, all statically linked, all pinned:

| Library | Used for | License |
| --- | --- | --- |
| Dear ImGui (docking) | UI | MIT |
| Zydis | Disassembly | MIT |
| Keystone | Assembly | **GPLv2** |
| Lua 5.4.7 | Scripting | MIT |
| Catch2 | Tests | BSL-1.0 |

Keystone is why the binary is GPLv2. It also vendors a C++14-era LLVM fork that
does not compile as C++20 (`std::unary_function`, `std::iterator`), which is why
this project sets **no global `CMAKE_CXX_STANDARD`** — first-party targets opt in
with `target_compile_features(... cxx_std_20)` and vendored code keeps its own
defaults. Likewise `/WX` is applied through the `pointerlab_warnings` interface
target, so third-party warnings never fail the build.

## Tests

`tests/` builds one Catch2 executable plus two helper processes:

- **`pointerlab_test_helper`** is a scriptable target: it owns a known value at a
  known address, a two-level pointer chain, and a hot function for breakpoints,
  and it answers commands over stdin. Scanning, writing and breakpointing all
  behave differently across a process boundary than in-process, so the
  integration tests use a real second process rather than targeting themselves.
- **`pointerlab_test_helper32`** is the same source compiled for Win32, built by
  an `ExternalProject` because MSVC's architecture comes from the generator
  rather than from a target property. It backs `test_wow64.cpp`. Sharing the
  source with the 64-bit helper is the point: the tests assert that the same
  program, built for x86, is scanned, chain-walked, disassembled and
  breakpointed correctly. It is optional
  (`-DPOINTERLAB_BUILD_HELPER32=OFF`), and its tests `SKIP` rather than fail
  when it is absent, so a missing optional fixture cannot be mistaken for
  coverage.
- **`pointerlab_crash_probe`** installs the crash handler and then crashes on
  purpose, with `%LOCALAPPDATA%` redirected to a scratch directory. It is the
  only way to test a code path whose successful outcome is the process ending.
