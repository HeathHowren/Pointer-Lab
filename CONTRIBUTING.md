# Contributing to Pointer Lab

Thanks for your interest. Pointer Lab is a Windows x64 memory-research tool, so
contributions carry a little more responsibility than usual: the code reads and
writes other processes' memory, and a mistake can crash somebody's target
process or their machine's stability.

## Licensing

Pointer Lab is **GPLv2** (see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). By submitting a contribution
you agree to license it under the GPLv2. Do not paste code from
incompatibly-licensed projects.

## Building

Requirements: Visual Studio 2022 (with the C++ workload), CMake 3.28+, and
Python 3 on `PATH`. Nothing in Pointer Lab uses Python; Keystone's vendored LLVM
runs a Python script during configure, and without an interpreter the configure
step aborts. CMake fetches every dependency automatically; the first configure
is slow because Keystone builds the LLVM MC layer.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Ground rules

1. **Warnings are errors.** First-party code builds with `/W4 /WX`. Do not
   silence a warning with a cast unless you have understood it; fix the cause.
   Third-party code is deliberately exempt.
2. **Check your Win32 return values.** Nearly every bug worth fixing in this
   codebase's history was an ignored `BOOL` or a dropped `HRESULT`.
   `infra::Result` is `[[nodiscard]]` — respect it.
3. **Never make the target process worse.** Anything that writes to, patches,
   or debugs the target must restore what it changed, including on the error
   path. Breakpoint code in particular must leave the instruction stream and
   thread context exactly as it found them.
4. **Surface errors to the user.** A silent failure is a bug. Route failures
   through the status/toast channel, not only the log.
5. **Confirm destructive actions.** Anything that allocates in, injects into,
   patches, or detaches from a live process needs a confirmation step.

   There is exactly one exception, and it is worth knowing about before you trip
   over it: the MCP server does not confirm anything. Starting it is the consent,
   and it covers every call that follows — a per-call prompt an agent hits every
   few seconds is one a person learns to click through, which is worse than no
   prompt because it looks like a control. That trade is only defensible while
   the warning before the Start button stays as blunt as it is, so treat that
   text as part of the feature. Do not extend the exception to anything else, and
   do not add a tool to the registry that has no equivalent path through the UI.
6. **Keep claims honest.** If a feature only handles part of a problem, say so
   in the UI and the README. We would rather ship a small true claim than a
   large false one.

## Testing

Pure logic (parsing, formatting, serialization, assembly/disassembly) belongs in
`tests/` with Catch2 and must come with a test. Code that needs a live process
is harder to test; at minimum, describe the manual verification you performed in
the pull request.

## Style

`.clang-format` and `.editorconfig` are in the repository root; match the
surrounding code. Four-space indent, no tabs.

## Reporting bugs

Include your Windows version, whether you ran elevated, the target process
architecture (x86 vs x64), and the relevant portion of
`%LOCALAPPDATA%\PointerLab\engine.log`. If Pointer Lab crashed, attach the
minidump written next to that log.
