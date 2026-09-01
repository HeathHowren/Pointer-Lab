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
- runs user-supplied Lua scripts in its own process,
- can, once you start it, expose all of the above over a loopback HTTP socket to
  an MCP client, with no per-call confirmation.

These are the intended features, not vulnerabilities. Reports amounting to
"this program can modify another program's memory" will be closed, and so will
"the MCP server lets a client patch a process without asking" — that is what the
panel says it does before you press Start.

## Threat model

Pointer Lab trusts the user running it and runs with that user's privileges. It
requests `SeDebugPrivilege` and works with reduced access when it cannot get it.
It does **not** attempt privilege escalation, install drivers, or persist.

Things that *are* in scope:

- Pointer Lab crashing, corrupting, or escalating privileges when opening a
  malformed `.iretable` project file or `imgui.ini` layout.
- The Lua sandbox permitting more than intended.
- Memory-safety bugs in Pointer Lab's own parsing of untrusted input
  (project files, script text, target memory contents, and MCP HTTP requests and
  JSON bodies — the one input that arrives over a socket).
- The MCP server accepting a request it should have refused: a connection from
  anything but the loopback address, a request with a wrong or absent bearer
  token, or a token recoverable from disk, from a log, or by timing the
  comparison.
- The MCP server still listening after it was stopped, or on a port other than
  the one shown in the panel.
- Anything that causes Pointer Lab to damage a target process it was not
  pointed at.

Explicitly **not** in scope: what an authorised MCP client does once it holds the
token. Every tool is a documented feature of the tool it is exposing.

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
- The MCP server is off unless you start it. While it is running, the token in
  the panel is the only thing between another program on your machine and a
  memory-write, code-patch and DLL-injection API — one that does not ask before
  any of it. Stop the server when you are done; detaching, loading a project and
  the target exiting all leave it running.
