# Pointer Lab

A Windows x64 user-mode memory research tool built in C++20 with a Dear ImGui dockspace UI.

![Pointer Lab](docs/screenshot.png)

*Attached to the bundled test helper, after an exact scan for a known i32. The
memory view shows the same address: `34 12 FE 5A` is `0x5AFE1234` little-endian.*

Pointer Lab attaches to a running 64-bit process, searches its memory for values,
tracks the addresses it finds, and lets you read, write, freeze, disassemble and
patch them. It is a research and learning tool — see
[Intended use](#intended-use) before pointing it at anything.

## Download

Grab the latest release from
[Releases](https://github.com/HeathHowren/Pointer-Lab/releases). The zip holds
`PointerLab.exe` alongside the licence, the third-party notices and a copy of
these docs — but the executable is the only part you need. There is no installer
and there are no runtime prerequisites: the C runtime is statically linked and
the fonts are embedded, so `PointerLab.exe` runs on its own from wherever you
put it.

Two things to expect on first run:

- **SmartScreen will warn you.** Release binaries are unsigned. Code-signing
  certificates cost money this project does not have.
- **Antivirus may flag it.** Reading and writing another process's memory,
  injecting a DLL and setting software breakpoints are exactly the behaviours
  heuristic scanners look for. That is the tool working as designed, not a
  reassurance that any given binary is safe — build from source if you would
  rather not take that on trust.

Run as administrator for full access. Without it Pointer Lab still works, but
`SeDebugPrivilege` is unavailable, many processes will open read-only, and the
command bar shows a **READ-ONLY** badge to say so rather than failing writes
silently.

**64-bit targets only.** Pointer Lab is a 64-bit process and cannot attach to
32-bit processes.

## Features

- **Scanner** — exact, unknown-initial, changed, unchanged, increased and
  decreased scans over signed and unsigned 8/16/32/64-bit integers, float,
  double and byte patterns.
  Relative modes capture a baseline on the first scan and filter on the next.
  Byte patterns support `??` wildcards (`48 8B ?? 24`). Float exact-match uses a
  configurable epsilon, because bit-exact float comparison finds nothing in
  practice. The result limit is configurable and truncation is reported rather
  than hidden.
- **Address list** — groups, descriptions, value freeze, manual add and edit, and
  F1–F12 freeze toggles registered with Windows, so they fire while the *target*
  is in the foreground. Only keys actually assigned to an entry are registered,
  so Pointer Lab does not take F1–F12 away from the rest of the machine.
- **Pointer scanner** — multi-level pointer chain search with a rescan pass.
  Chains are stored as `module+offset` plus offsets, so a chain found in one run
  still resolves after the target restarts and ASLR moves everything. Restart the
  target, find the value's new address and rescan: the chains that still resolve
  to it are the ones that genuinely track the value, and the thousands that only
  pointed the right way once are discarded. Resolved chains can be added to the
  address list as live tracked entries.
- **Memory viewer** — hex display with live patching.
- **Disassembler** — full x86-64 disassembly via [Zydis](https://github.com/zyantific/zydis),
  with follow-branch navigation. Undecodable bytes are shown as `db` rather than
  desynchronising the listing.
- **Assembler** — full x86-64 assembly via [Keystone](https://github.com/keystone-engine/keystone).
  Patches are NOP-padded to the next instruction boundary so a short patch never
  leaves half an instruction behind, and the confirmation dialog tells you
  exactly how many bytes will be overwritten.
- **Breakpoints** — repeatable user-mode breakpoints via the Windows debug APIs,
  so the target keeps running and the hit count climbs. Register context is
  captured on hit. Software (`int3`) breakpoints rewind RIP, single-step and
  re-arm; hardware breakpoints use the CPU's four debug registers instead, never
  modify the target and are never disarmed, and can break on data being written
  or read rather than only on code being executed.
- **Injection** — remote allocation, remote thread creation and `LoadLibraryW`
  helpers, each behind a confirmation dialog.
- **Lua scripting** — embedded Lua 5.4 console running off the UI thread, with a
  cancel button that actually interrupts a runaway script. See
  [docs/lua-api.md](docs/lua-api.md).
- **Persistence** — `.iretable` project format with File → New/Open/Save/Save As,
  plus autosave on exit and autoload on start. Format documented in
  [docs/iretable-format.md](docs/iretable-format.md). Scan options and which
  panels are open are remembered separately, in a plain `settings.ini` beside the
  logs, because they belong to the installation rather than to a project.
- **Diagnostics** — structured log with per-thread ids and a level filter, and
  automatic minidumps on crash (including uncaught C++ exceptions, not just
  access violations).

### Known limits

Stated plainly, because a tool that overstates itself wastes your time:

- A software breakpoint can be missed by another thread during the single-step
  window in which it is temporarily disarmed. This is inherent to `int3`
  breakpoints and is not specific to Pointer Lab; use a hardware breakpoint,
  which is never disarmed, when it matters.
- There are only four hardware breakpoints, because the processor has four debug
  registers. The fifth is refused rather than silently replacing one.
- Breakpoint hit *notifications* are rate-limited to one every 500 ms so a
  breakpoint in a hot loop cannot flood the UI. The hit count in the breakpoint
  table is the authoritative record.
- Partial reads across partly-mapped regions succeed with a shortened buffer
  rather than failing outright, which is what lets a scan cross them.
- There is no kernel driver, no anti-anti-cheat, and no attempt at stealth.

## Intended use

Pointer Lab is for inspecting software **you own or are authorised to test** —
your own programs, single-player games, CTF binaries, and reverse-engineering
practice.

Using it against online or competitive games will very likely trip anti-cheat
software and get the account banned. Using it against software you do not have
permission to modify may be illegal where you live. That decision is yours; this
tool does not make it for you.

## Building from source

**Requirements:** Visual Studio 2022 (MSVC v143), CMake 3.28+, and **Python 3**
on `PATH`. Python is not used by Pointer Lab itself — Keystone vendors an LLVM
fork whose CMake calls a Python script during *configure*, so without an
interpreter the first command below fails before anything is compiled. The
Visual Studio C++ workload does not install one.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The first configure downloads and builds Dear ImGui, Lua 5.4.7, Zydis, Keystone
and Catch2 via `FetchContent`, all pinned by tag or commit. Keystone brings the
LLVM MC layer with it and dominates the first build; it is cached afterwards.

To produce the release zip:

```powershell
cpack --config build/CPackConfig.cmake -C Release -B build/package
```

## Documentation

- [Architecture](docs/architecture.md) — how the layers fit together
- [Lua API reference](docs/lua-api.md)
- [`.iretable` format](docs/iretable-format.md)
- [Changelog](CHANGELOG.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

## License

Pointer Lab is licensed under the **GNU General Public License v2.0** — see
[LICENSE](LICENSE). GPLv2 rather than something more permissive because Keystone
is GPLv2 and is statically linked into the binary.

Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
