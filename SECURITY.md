# Security Policy

## Reporting a vulnerability

Please report security issues privately through GitHub's
[private vulnerability reporting](https://github.com/HeathHowren/Pointer-Lab/security/advisories/new)
rather than opening a public issue. Include reproduction steps and the affected
version. Expect an initial response within a few days.

## What Pointer Lab is

Pointer Lab is a debugging and memory-research tool. By design it:

- opens handles to other processes and reads and writes their memory,
- attaches as a debugger and sets software breakpoints,
- allocates memory in, and creates threads in, other processes,
- loads DLLs into other processes,
- runs user-supplied Lua scripts in its own process.

These are the intended features, not vulnerabilities. Reports amounting to
"this program can modify another program's memory" will be closed.

## Threat model

Pointer Lab trusts the user running it and runs with that user's privileges. It
requests `SeDebugPrivilege` and works with reduced access when it cannot get it.
It does **not** attempt privilege escalation, install drivers, or persist.

Things that *are* in scope:

- Pointer Lab crashing, corrupting, or escalating privileges when opening a
  malformed `.iretable` project file or `imgui.ini` layout.
- The Lua sandbox permitting more than intended.
- Memory-safety bugs in Pointer Lab's own parsing of untrusted input
  (project files, script text, target memory contents).
- Anything that causes Pointer Lab to damage a target process it was not
  pointed at.

## Notes for users

- Release binaries are **unsigned**. SmartScreen will warn on first run, and
  antivirus or anti-cheat software may flag the executable because its whole
  purpose is inspecting other processes. Verify you downloaded it from the
  official releases page.
- Only use Pointer Lab on software you own or are authorised to analyse. Using
  it against online games or other people's systems may violate the terms of
  service or the law where you live.
- Treat `.iretable` project files and Lua scripts from other people as
  untrusted: a script has the same access to your machine that Pointer Lab does.
