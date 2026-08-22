// Everything that reaches the disk: project files, the autosaved session and
// the settings that persist between runs.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "infra/Paths.h"
#include "infra/Settings.h"

#include <commdlg.h>

namespace ire::ui {

namespace {

// Minimal wrapper over the common item dialog. Returns an empty path when the
// user cancels.
std::filesystem::path runFileDialog(HWND owner, bool saving, const std::filesystem::path& initial) {
    std::array<wchar_t, 1024> buffer{};
    if (!initial.empty()) {
        const auto text = initial.wstring();
        const auto count = std::min(text.size(), buffer.size() - 1);
        std::copy_n(text.begin(), count, buffer.begin());
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Pointer Lab project (*.iretable)\0*.iretable\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrDefExt = L"iretable";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.Flags |= saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

    const BOOL chosen = saving ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!chosen) {
        return {};
    }
    return std::filesystem::path(buffer.data());
}

} // namespace

std::string UiApp::projectTitle() const {
    return projectPath_.empty() ? std::string("Untitled project") : projectPath_.filename().string();
}

void UiApp::newProject() {
    confirmAction("Start a new project?",
                  "This clears the address list. Anything not saved is lost.",
                  "New project",
                  [this] {
                      services_.session().addressList().replace({});
                      projectPath_.clear();
                      notifyInfo("Started a new project.");
                  });
}

bool UiApp::saveProjectTo(const std::filesystem::path& path, bool quiet) {
    storage::ProjectTable table;
    table.lastPid = services_.session().pid();
    table.lastProcessName = services_.session().processName();
    table.entries = services_.session().addressList().snapshot();

    auto saved = projectStore_.save(path, table);
    if (!saved) {
        notifyError("Could not save " + path.filename().string() + ": " + saved.error());
        return false;
    }
    if (!quiet) {
        notifyInfo("Saved " + std::to_string(table.entries.size()) + " entries to " + path.filename().string() + ".");
    }
    return true;
}

bool UiApp::loadProjectFrom(const std::filesystem::path& path, bool quiet) {
    auto loaded = projectStore_.load(path);
    if (!loaded) {
        if (!quiet) {
            notifyError("Could not open " + path.filename().string() + ": " + loaded.error());
        }
        return false;
    }

    const auto count = loaded.value().entries.size();
    services_.session().addressList().replace(std::move(loaded.value().entries));
    if (!quiet) {
        notifyInfo("Loaded " + std::to_string(count) + " entries from " + path.filename().string() + ".");
    }
    return true;
}

void UiApp::saveProject() {
    if (projectPath_.empty()) {
        saveProjectAs();
        return;
    }
    saveProjectTo(projectPath_, false);
}

void UiApp::saveProjectAs() {
    const auto path = runFileDialog(hwnd_, true, projectPath_);
    if (path.empty()) {
        return;
    }
    if (saveProjectTo(path, false)) {
        projectPath_ = path;
    }
}

void UiApp::openProjectDialog() {
    const auto path = runFileDialog(hwnd_, false, {});
    if (path.empty()) {
        return;
    }
    if (loadProjectFrom(path, false)) {
        projectPath_ = path;
    }
}

void UiApp::loadSession() {
    const auto path = infra::Paths::sessionFile();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    // Quiet: an unreadable autosave should not greet the user with an error.
    if (loadProjectFrom(path, true)) {
        infra::Logger::instance().info("Restored the previous session.");
    }
}

void UiApp::saveSession() {
    if (services_.session().addressList().snapshot().empty()) {
        return;
    }
    saveProjectTo(infra::Paths::sessionFile(), true);
}

void UiApp::loadSettings() {
    const auto settings = infra::loadSettings(infra::Paths::settingsFile());

    scanMaxResults_ = static_cast<int>(
        std::clamp<std::uint64_t>(settings.scanMaxResults, 1000, 20000000));
    scanFloatEpsilon_ = std::clamp(static_cast<float>(settings.scanFloatEpsilon), 0.0f, 1000.0f);
    scanWritableOnly_ = settings.scanWritableOnly;
    scanExecutableOnly_ = settings.scanExecutableOnly;

    // A stored index is clamped rather than trusted: the list of value types can
    // grow between releases, and an out-of-range index would read off the end.
    const auto typeCount = static_cast<int>(domain::valueTypes().size()) - 1;
    scanTypeIndex_ = std::clamp(settings.scanTypeIndex, 0, typeCount);
    pointerTypeIndex_ = std::clamp(settings.pointerTypeIndex, 0, typeCount);
    pointerDepth_ = std::clamp(settings.pointerDepth, 1, 8);

    showMemoryViewer_ = settings.showMemoryViewer;
    showDisassembly_ = settings.showDisassembly;
    showBreakpoints_ = settings.showBreakpoints;
    showModules_ = settings.showModules;
    showMemoryRegions_ = settings.showMemoryRegions;
    showLogs_ = settings.showLogs;
    showPointerScanner_ = settings.showPointerScanner;
    showLuaScanner_ = settings.showLuaScanner;
    showInjection_ = settings.showInjection;
    showLuaConsole_ = settings.showLuaConsole;

    // The scan job holds its own copy, so restoring the fields alone would leave
    // the first scan of the session running with default options.
    engine_scan::ScanOptions options;
    options.writableOnly = scanWritableOnly_;
    options.executableOnly = scanExecutableOnly_;
    options.maxResults = static_cast<std::size_t>(scanMaxResults_);
    options.floatEpsilon = static_cast<double>(scanFloatEpsilon_);
    services_.scanJob().setOptions(options);
}

void UiApp::saveSettings() {
    infra::Settings settings;
    settings.scanMaxResults = static_cast<std::uint64_t>(scanMaxResults_);
    settings.scanFloatEpsilon = static_cast<double>(scanFloatEpsilon_);
    settings.scanWritableOnly = scanWritableOnly_;
    settings.scanExecutableOnly = scanExecutableOnly_;
    settings.scanTypeIndex = scanTypeIndex_;
    settings.pointerDepth = pointerDepth_;
    settings.pointerTypeIndex = pointerTypeIndex_;
    settings.showMemoryViewer = showMemoryViewer_;
    settings.showDisassembly = showDisassembly_;
    settings.showBreakpoints = showBreakpoints_;
    settings.showModules = showModules_;
    settings.showMemoryRegions = showMemoryRegions_;
    settings.showLogs = showLogs_;
    settings.showPointerScanner = showPointerScanner_;
    settings.showLuaScanner = showLuaScanner_;
    settings.showInjection = showInjection_;
    settings.showLuaConsole = showLuaConsole_;

    if (!infra::saveSettings(infra::Paths::settingsFile(), settings)) {
        // Nothing is visible by now -- the window is gone -- so the log is the
        // only place this can be said, but it must still be said somewhere.
        infra::Logger::instance().error("Could not write " + infra::Paths::settingsFile().string() +
                                        "; preferences from this session were not saved.");
    }
}

} // namespace ire::ui
