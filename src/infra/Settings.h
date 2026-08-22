#pragma once

#include <cstdint>
#include <filesystem>

namespace ire::infra {

// What Pointer Lab remembers between runs that is not an address list.
//
// Deliberately not part of the .iretable project format. These are preferences
// belonging to the installation rather than to a project: a result limit is not
// something you want to travel with a project file to somebody else's machine,
// and putting them there would mean every saved project carried a copy.
//
// Every field has the value the application used before any of this was
// persisted, so a missing settings file behaves exactly as the previous release
// did.
struct Settings {
    // Scan options, mirroring engine_scan::ScanOptions.
    std::uint64_t scanMaxResults{1000000};
    double scanFloatEpsilon{0.0001};
    bool scanWritableOnly{};
    bool scanExecutableOnly{};
    int scanTypeIndex{4};

    // Pointer scanner defaults.
    int pointerDepth{3};
    int pointerTypeIndex{4};

    // Which panels are open. ImGui's own .ini remembers where a panel sits and
    // what it is docked to, but whether it is open at all is the application's
    // state, so a panel the user closed used to come back on every launch.
    bool showMemoryViewer{true};
    bool showDisassembly{true};
    bool showBreakpoints{true};
    bool showModules{true};
    bool showMemoryRegions{true};
    bool showLogs{true};
    bool showPointerScanner{true};
    bool showLuaScanner{true};
    bool showInjection{true};
    bool showLuaConsole{true};
};

// Never fails. A missing, unreadable or malformed file yields defaults, and an
// unparseable line is skipped rather than discarding the rest of the file --
// losing a preference is not worth refusing to start over.
[[nodiscard]] Settings loadSettings(const std::filesystem::path& path);

// Reports failure so the caller can say so rather than silently not saving.
[[nodiscard]] bool saveSettings(const std::filesystem::path& path, const Settings& settings);

} // namespace ire::infra
