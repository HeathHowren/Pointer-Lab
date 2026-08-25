# Changelog

All notable changes to Pointer Lab are recorded here. This project follows
[Semantic Versioning](https://semver.org/).

## [2.1.0] — 2026-08-22

A feature release, and the first one that is not mostly repair work. Every item
in it closes a gap that 2.0.0 documented as a known limit rather than fixed.

Nothing in it is breaking: `.iretable` project files are still **format version
3**, the licence is unchanged, and every existing workflow behaves as it did.
Software breakpoints remain the default, and settings live in their own file
rather than in the project format.

### Added

- **Pointer scan rescan pass.** The pointer scanner can now narrow the chains it
  already found instead of only starting over. Restart the target, find the
  value's new address, put it in Target address and press Rescan: every chain is
  re-resolved and only the ones that still land on the value are kept. A first
  scan returns thousands of chains that pointed the right way once; this is what
  reduces them to the handful that actually track the value. It costs a few reads
  per chain rather than another sweep of the address space, so it takes seconds
  where the first scan took minutes.

  A rescan refuses, and leaves the chains untouched, when nothing is attached or
  there is nothing to narrow — resolution fails for every chain with no process
  attached, so running it anyway would silently discard a scan that took minutes.
  Cancelling a rescan also leaves the chains untouched, because a half-applied
  filter is not a result set.

- **Hardware breakpoints and watchpoints.** A breakpoint can now use one of the
  processor's four debug registers instead of an `int3`. Nothing in the target is
  modified and nothing is ever disarmed, which closes the window a software
  breakpoint can be missed through: another thread running the address while the
  original byte is temporarily back no longer slips past. They also break on
  **data** rather than only on code — watch an address for writes, or for reads
  and writes, in widths of 1, 2, 4 or 8 bytes.

  There are exactly four, because the processor has four debug registers. The
  fifth is refused with a message that says so rather than silently displacing
  one, and the breakpoint table names the register each one holds. Software
  breakpoints are unchanged and remain the default.

- **Global hotkeys.** F1–F12 freeze toggles are now registered with Windows
  instead of being read from Pointer Lab's own message queue, so they fire while
  the target window is in the foreground. That is the only time they were ever
  wanted: the previous in-window handling stopped working the moment you clicked
  into the target.

  Only keys actually assigned to an address list entry are registered, so
  Pointer Lab does not take F1–F12 away from every other application for as long
  as it is open. A key another application already owns cannot be registered; it
  is reported and falls back to the old in-window behaviour rather than silently
  doing nothing.

- **Settings persistence.** Scan options (result limit, float tolerance, the
  writable and executable filters), the selected value types, the pointer scan
  depth and which panels are open now survive a restart. ImGui's own `.ini`
  already remembered where a panel sat and what it was docked to, but whether it
  was open at all is the application's state, so a panel you closed came back on
  every launch.

  These live in a plain `settings.ini` next to the log, not in `.iretable`: they
  belong to the installation rather than to a project, and a result limit has no
  business travelling with a project file to somebody else's machine. **The
  project format is unchanged and still version 3.**

  A missing, malformed or newer-than-expected settings file falls back to the
  values the previous release used, and one unparseable line does not discard the
  rest of the file.

- **Lua `cancelled()` and `check_cancel()`.** A script can now see that Stop has
  been pressed, which matters in a loop that spends its time inside Pointer Lab's
  own functions where the VM hook rarely gets a turn. `check_cancel()` ends the
  script there and then; like Stop itself, it cannot be caught.

### Changed

- **`loadlibrary()` returns the module's base address** rather than the remote
  thread's exit code, which was only the low 32 bits of the module handle and so
  looked like an address on a 64-bit target without being one. The module list is
  refreshed as part of the call, so a newly loaded DLL is visible to `modules()`
  immediately. If the module cannot be found afterwards the old exit code is
  returned rather than reporting a failure that did not happen.

### Fixed

- **A cancelled Lua script can no longer catch its own cancellation.** Stop
  raised a Lua error, and `pcall` catches errors, so
  `while true do pcall(f) end` swallowed the cancel and carried on — erroring
  again every 10 000 instructions and never stopping. A runaway loop is exactly
  what Stop is for. Scripts now run on a coroutine and cancelling yields instead
  of raising; a yield passes straight through `pcall` and `xpcall`, and the
  coroutine is simply never resumed.

- **Stopping a script now stops the scan it started.** The script ended but its
  scan carried on in the background, so Stop did not stop the work, and the
  results turned up in the next script that looked.

### Internal

- **`UiApp.cpp` is split into nine files grouped by area.** It was a single
  2500-line file, roughly five times the size of the next largest, and the one
  component with no test coverage. The class and its header are unchanged and
  every panel is still one method; only which file each lives in has moved. The
  helpers the panels share moved from an anonymous namespace into
  `src/ui/UiInternal.h`.

### Known limits

Still true, and still stated rather than hidden — see the README for the full
list. A software breakpoint can be missed during its single-step window, which is
inherent to `int3`; use a hardware breakpoint when it matters. There are only
four hardware breakpoints, because the processor has four debug registers.
Breakpoint notifications are rate limited (hit counts are not). Time inside a
Lua C function is still not interruptible. Partial reads succeed with a short
buffer. 64-bit targets only.

## [2.0.0] — 2026-08-18

The first release intended for real use. The alpha built and ran, but a number
of its advertised features were stubs that returned nothing, and its breakpoints
crashed the target. Everything below is either a fix for that or the work needed
to make the claims in the README true.

A major version because two things changed that are not backward compatible:
Pointer Lab is now **licensed under the GPLv2** rather than being unlicensed, and
`.iretable` project files are now written in **format version 3**, which earlier
releases cannot read. (Pointer Lab still reads versions 1 and 2.)

### Added

- **Project persistence.** File → New / Open / Save / Save As, autosave on exit
  and autoload on start. `ProjectStore` existed in the alpha
  but was called from nowhere, so the address list was lost on every exit. The
  format now records pointer chains and is documented in
  [docs/iretable-format.md](docs/iretable-format.md).
- **Real disassembler.** Zydis replaces the roughly twenty-opcode hand-rolled
  decoder. Instruction lengths are correct, so the listing no longer desyncs;
  undecodable bytes are shown as `db` and advance one byte. Branch targets are
  resolved and followable.
- **Real assembler.** Keystone replaces the nine-mnemonic parser. Patches are
  NOP-padded to the next instruction boundary so a short patch cannot leave half
  an instruction behind.
- **Working breakpoints.** Software breakpoints are now repeatable: RIP rewind,
  original-byte restore, trap-flag single-step and re-arm. Register context is
  captured on hit and shown in the UI. Hit counts climb and the target keeps
  running.
- **Relative scan modes as first scans.** Changed / Unchanged / Increased /
  Decreased capture a baseline instead of matching nothing, and Next scan works
  with an empty value box — the unknown-value workflow that is the reason those
  modes exist.
- **Wildcard byte patterns.** `48 8B ?? 24` now works; `??` was previously
  stripped and ignored.
- **Float epsilon matching.** Bit-exact float comparison finds nothing in
  practice, so exact float scans now use a configurable tolerance.
- **Pointer chains as live addresses.** A found chain can be added to the address
  list, is stored as `module+offset`, is re-resolved about twice a second, and
  survives both a save/load cycle and a target restart under ASLR. Previously
  "Add base" discarded the offsets.
- **Lua console off the UI thread**, with a working cancel — an infinite loop no
  longer hangs the application permanently. Added scan-result access so scripted
  scans can be chained, typed reads and writes, module and region enumeration,
  and pointer-chain resolution. Errors come back with a traceback.
- **Confirmation dialogs** on every destructive action: remote RWX allocation,
  remote thread creation, DLL injection, assemble-and-patch, raw patch, entry
  removal, breakpoint removal, and detaching with live breakpoints. The alpha had no
  dialogs at all.
- **Error surfacing.** Failures raise a toast and are logged; previously they
  went only to a hidden log panel, and many were discarded entirely.
- **Crash minidumps.** `MiniDumpWriteDump` on unhandled exceptions, uncaught C++
  throws, invalid CRT parameters and pure-call violations, with a message naming
  the dump. Help → Write a diagnostic dump captures a dump without crashing.
- **About and Help dialogs**, including the keybindings, the Lua API and the
  legal/ethical-use notice.
- **Tests and CI.** 80 Catch2 tests, including integration tests against a real
  spawned target process, an end-to-end workflow test that scans, freezes,
  saves, restarts the target and re-resolves, and crash-path tests against a
  process that crashes on purpose. GitHub Actions builds and tests Debug and
  Release, packages the release zip, and attaches it to tagged releases.
- **LICENSE (GPLv2), THIRD_PARTY_NOTICES, CONTRIBUTING, SECURITY**, version
  resources, an application manifest, and CPack packaging.

### Fixed

- **Breakpoints crashed the target.** Execution resumed in the middle of the
  overwritten instruction. This was the most serious bug in the alpha.
- **Detaching or removing a breakpoint could kill the target.** An `int3` already
  in flight was delivered to a process that no longer had a debugger. Fixed by
  recognising stale traps and draining queued events before releasing.
- **Use-after-free in DLL injection** — the remote path buffer was freed before
  the wait completed.
- **Partial `ReadProcessMemory` was reported as complete success.**
- **`parseAddress` misread un-prefixed hex**, affecting every address field.
- **Missing font files null-dereferenced on startup.** Fonts are now embedded, so
  the executable is genuinely portable.
- **64-bit identifiers were truncated to `int`** for ImGui IDs, so rows more than
  4 GB apart could collide.
- **Chunk-boundary duplicates and a wrong progress denominator** in the scanner;
  a cap that silently aborted an unknown scan after about 1 MB.
- **The pointer scanner's linear frontier search**, which made anything past
  depth 1 effectively never finish. It now uses a sorted target set with a binary
  search, plus dedup and a cycle guard.
- **Unguarded `std::stoull` in the project loader**, which threw out of `load()`
  on a malformed file — and because an uncaught C++ exception bypassed the SEH
  filter, produced no crash log either.
- **Dropped Win32 and D3D return values**, including `VirtualProtectEx`.
  Device-removed (TDR) is now recovered from, and rendering is skipped when
  minimized.
- **Write failures in the freeze loop were discarded silently.** A failing entry
  is now disabled and reported.
- **A target-handle lifetime race** between the freeze loop and `detach()`.
- **Most of the UI was invisible on a first run.** Ten of the thirteen panels
  defaulted to hidden and had no place in the default layout, so a new user saw
  the process list, the scanner and the address list and nothing else. Every
  panel now opens by default with its own home in the layout, and Reset Layout
  reopens the ones you closed.
- **The default dock layout was overridden on the frame it was built**, which
  collapsed six panels into two tab bars even once they were visible.
- **The log file was reopened for every line** and truncated on launch,
  destroying the evidence from the run that crashed. It is now held open and
  rotated.
- Swapped ternary in the runtime services status text; non-atomic pump flags;
  ImGui and DX11 shutdown running when `run()` bailed before initialisation.

### Changed

- **Licensed under GPLv2** (was unlicensed), because Keystone is GPLv2 and is
  statically linked.
- **Statically linked C runtime.** The alpha claimed "no dependencies" while
  requiring the VC++ redistributable.
- **Scan result limit is configurable and truncation is reported** as "N of M"
  rather than silently capping.
- **Read-only attachment is surfaced** with a badge instead of a silent
  half-attach with failing writes.
- **`SeDebugPrivilege` is acquired at startup** when available.
- Log panel gained per-thread ids, a level filter, a text filter and a link to
  the log folder; the scanner, pointer scanner and project store now log what
  they do.
- README rewritten to describe what the tool actually does, including a
  **Known limits** section.

### Known limits

Carried forward deliberately rather than hidden — see the README for the full
list. In short: no pointer-scan rescan pass; a software breakpoint can be missed
by another thread during its single-step window; breakpoint notifications are
rate limited (hit counts are not); partial reads succeed with a short buffer;
64-bit targets only.

## [1.0.1] — 2026-05-13

README cleanup for the alpha release. No code changes.

Note for anyone comparing version numbers: the tree's own
`project(PointerLab VERSION ...)` still read `0.1.0` at this point, while the
published tags had moved to `1.0.x`. 2.0.0 is the first release where the
version in the source, the version resource in the executable, the packaged zip
and the git tag all agree.

## [1.0.0-alpha] — 2026-05-13

Initial alpha. Built and launched, with a working process browser, exact-value
scanner, address list, memory viewer and ImGui dockspace UI.
