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

One class, one file, panel-per-method. It owns the Win32 window, the D3D11
device and the ImGui context, and holds `RuntimeServices` by reference.

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
- **`pointerlab_crash_probe`** installs the crash handler and then crashes on
  purpose, with `%LOCALAPPDATA%` redirected to a scratch directory. It is the
  only way to test a code path whose successful outcome is the process ending.
