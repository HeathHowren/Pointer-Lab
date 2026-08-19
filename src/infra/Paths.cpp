#include "infra/Paths.h"

#include <Windows.h>

#include <array>

namespace ire::infra {

std::filesystem::path Paths::appData() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    std::filesystem::path base = n > 0 ? std::filesystem::path(buffer.data()) : std::filesystem::temp_directory_path();
    return base / "PointerLab";
}

std::filesystem::path Paths::logFile() {
    return appData() / "engine.log";
}

std::filesystem::path Paths::layoutFile() {
    return appData() / "imgui.ini";
}

std::filesystem::path Paths::sessionFile() {
    return appData() / "session.iretable";
}

std::filesystem::path Paths::crashFile() {
    return appData() / "crash.log";
}

std::filesystem::path Paths::crashDumpFile() {
    return appData() / "crash.dmp";
}

} // namespace ire::infra

