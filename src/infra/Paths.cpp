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

std::filesystem::path Paths::settingsFile() {
    return appData() / "settings.ini";
}

std::filesystem::path Paths::crashFile() {
    return appData() / "crash.log";
}

std::filesystem::path Paths::crashDumpFile() {
    return appData() / "crash.dmp";
}

std::filesystem::path Paths::installDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer.data(), n)).parent_path();
}

std::filesystem::path Paths::speedPayload(bool target64Bit) {
    return installDirectory() / (target64Bit ? L"PointerLabSpeed64.dll" : L"PointerLabSpeed32.dll");
}

} // namespace ire::infra

