#pragma once

#include <filesystem>

namespace ire::infra {

class Paths {
public:
    static std::filesystem::path appData();
    static std::filesystem::path logFile();
    static std::filesystem::path layoutFile();
    static std::filesystem::path sessionFile();
    static std::filesystem::path crashFile();
    static std::filesystem::path crashDumpFile();
};

} // namespace ire::infra

