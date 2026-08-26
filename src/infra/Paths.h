#pragma once

#include <filesystem>

namespace ire::infra {

class Paths {
public:
    static std::filesystem::path appData();
    static std::filesystem::path logFile();
    static std::filesystem::path layoutFile();
    static std::filesystem::path sessionFile();
    static std::filesystem::path settingsFile();
    static std::filesystem::path crashFile();
    static std::filesystem::path crashDumpFile();

    // The directory the running executable lives in, which is where anything
    // shipped alongside it -- the speed-hack payloads, the tutorial target --
    // is found. Distinct from appData(), which is where things this
    // installation *writes* go: a portable install may sit on a read-only
    // share, and the two must not be confused.
    static std::filesystem::path installDirectory();
    // The speed-hack payload for a target of that width. It has to match the
    // target, not this process: a 64-bit DLL cannot be loaded into a 32-bit
    // game at all.
    static std::filesystem::path speedPayload(bool target64Bit);
};

} // namespace ire::infra

