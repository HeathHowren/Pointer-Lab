# Pointer Lab

A Windows x64 user-mode memory research tool built in C++20 with a Dear ImGui dockspace UI.

![Pointer Lab](docs/screenshot.png)

## Download

Grab the latest `PointerLab.exe` from [Releases](https://github.com/HeathHowren/Pointer-Lab/releases). No installer, no dependencies — run directly. Administrator privileges recommended for full process access.

## Features

- **Scanner** — exact, unknown-initial, changed, unchanged, increased, and decreased scans across i8/i16/i32/i64/f32/f64 and byte patterns
- **Address list** — groups, descriptions, value freeze, manual add/edit, F1–F12 in-window hotkey toggles
- **Pointer scanner** — module-relative multi-level pointer chain search
- **Memory viewer** — hex display with live patching
- **Disassembler / Assembler** — x86/x64 disassembly listing and assembler patch panel
- **Breakpoints** — user-mode software breakpoints via Windows debug APIs
- **Injection** — remote allocation, remote thread creation, LoadLibraryW helpers
- **Lua scripting** — embedded Lua 5.4 console with a process/scan/address automation API
- **Persistence** — native `.iretable` project format, session file, ImGui layout, structured logging, crash log

## Building from source

**Requirements:** Visual Studio 2022, CMake 3.24+

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake fetches [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) and Lua 5.4.7 automatically. No other dependencies.
