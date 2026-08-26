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
                  "This clears the address list, the scripts and the structures. Any script that is "
                  "currently on is switched off first, so the target is put back as it was. Anything not "
                  "saved is lost.",
                  "New project",
                  [this] {
                      services_.session().addressList().replace({});
                      if (auto disabled = services_.autoAssembler().disableAll(); !disabled) {
                          notifyError("Some scripts could not be switched off: " + disabled.error());
                      }
                      services_.autoAssembler().forgetAll();
                      editScriptId_ = 0;
                      services_.dissector().forgetAll();
                      structureId_ = 0;
                      editFieldOffset_.reset();
                      projectPath_.clear();
                      notifyInfo("Started a new project.");
                  });
}

bool UiApp::saveProjectTo(const std::filesystem::path& path, bool quiet) {
    storage::ProjectTable table;
    table.lastPid = services_.session().pid();
    table.lastProcessName = services_.session().processName();
    table.lastBitness = services_.session().bitness();
    table.entries = services_.session().addressList().snapshot();
    for (const auto& symbol : services_.symbols().symbols()) {
        // Only the ones defined from an expression. A symbol pinned to a bare
        // address means nothing in the next run, and saving it would hand the
        // reader a name that points somewhere arbitrary.
        if (!symbol.expression.empty()) {
            table.symbols.push_back({symbol.name, symbol.expression});
        }
    }
    for (const auto& script : services_.autoAssembler().scripts()) {
        table.scripts.push_back({script.name, script.source});
    }
    // A structure with no fields is a name and nothing else; saving it would
    // only put an empty row in front of whoever opens the file next.
    for (const auto& structure : services_.dissector().structures()) {
        if (!structure.fields.empty()) {
            table.structures.push_back(structure);
        }
    }

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
    const auto tableBitness = loaded.value().lastBitness;

    // Re-resolved against the target that is attached now, not restored to the
    // address they had when the file was written -- which is the whole reason
    // the expression is what gets saved. A symbol whose module is not loaded is
    // named in the log rather than silently dropped.
    services_.symbols().clear();
    std::size_t unresolvedSymbols{};
    for (const auto& symbol : loaded.value().symbols) {
        if (auto defined = services_.symbols().define(services_.session(), symbol.name, symbol.expression);
            !defined) {
            infra::Logger::instance().warn("Could not resolve symbol \"" + symbol.name + "\" (" +
                                           symbol.expression + "): " + defined.error());
            ++unresolvedSymbols;
        }
    }
    if (unresolvedSymbols > 0 && !quiet) {
        notifyError(std::to_string(unresolvedSymbols) +
                    " symbol(s) could not be resolved against this target; see the log.");
    }

    // Scripts belonging to the outgoing project are switched off before they are
    // dropped, so loading a table never abandons a patch with nothing left that
    // knows how to undo it. They arrive switched off: nothing has been written
    // yet, and the user gets to read a script before it runs.
    if (auto disabled = services_.autoAssembler().disableAll(); !disabled && !quiet) {
        notifyError("Some scripts from the previous project could not be switched off: " + disabled.error());
    }
    services_.autoAssembler().forgetAll();
    editScriptId_ = 0;
    for (const auto& script : loaded.value().scripts) {
        services_.autoAssembler().add(script.name, script.source);
    }
    if (!loaded.value().scripts.empty()) {
        showScripts_ = true;
    }

    // Structure ids are assigned by the dissector, not carried in the file:
    // nothing outside a single run refers to one, and generating them here
    // keeps two tables loaded in sequence from colliding.
    services_.dissector().forgetAll();
    structureId_ = 0;
    editFieldOffset_.reset();
    std::size_t unusableStructures{};
    for (const auto& structure : loaded.value().structures) {
        const auto id = services_.dissector().add(structure.name);
        if (auto applied = services_.dissector().setFields(id, structure.fields); !applied) {
            infra::Logger::instance().warn("Structure \"" + structure.name +
                                           "\" could not be loaded: " + applied.error());
            ++unusableStructures;
        }
    }
    if (unusableStructures > 0 && !quiet) {
        notifyError(std::to_string(unusableStructures) +
                    " structure(s) had overlapping or zero-width fields and were left empty; see the log.");
    }
    if (!loaded.value().structures.empty()) {
        showStructures_ = true;
    }

    services_.session().addressList().replace(std::move(loaded.value().entries));
    if (!quiet) {
        notifyInfo("Loaded " + std::to_string(count) + " entries from " + path.filename().string() + ".");
    }

    // Offsets in a pointer chain were measured against a particular struct
    // layout, and that layout is not the same in a 32- and a 64-bit build of
    // the same program: every embedded pointer changes size. Such a chain still
    // resolves -- to the wrong field -- so this has to be said out loud rather
    // than left for the user to discover through a value that looks almost right.
    if (services_.session().attached() && tableBitness != services_.session().bitness()) {
        notifyError(std::string("This table was built against a ") + domain::bitnessName(tableBitness) +
                    " target, but the attached process is " + domain::bitnessName(services_.session().bitness()) +
                    ". Pointer chains will resolve to the wrong offsets.");
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
    // Nothing worth restoring only if *both* halves are empty. Checking the
    // address list alone used to throw away a session whose whole content was a
    // script.
    if (services_.session().addressList().snapshot().empty() &&
        services_.autoAssembler().scripts().empty() &&
        services_.dissector().structures().empty()) {
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
