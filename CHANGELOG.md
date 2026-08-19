# Changelog

All notable changes to Pointer Lab are recorded here. This project follows
[Semantic Versioning](https://semver.org/).

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
