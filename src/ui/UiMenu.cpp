// The menu bar and the command bar above the panels.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "infra/CrashHandler.h"
#include "infra/Paths.h"

#include <shellapi.h>

namespace ire::ui {

void UiApp::renderMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New project")) {
            newProject();
        }
        if (ImGui::MenuItem("Open project...")) {
            openProjectDialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save project")) {
            saveProject();
        }
        if (ImGui::MenuItem("Save project as...")) {
            saveProjectAs();
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", projectTitle().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset default layout")) {
            resetDockLayout_ = true;
        }
        ImGui::MenuItem("Show manual address editor", nullptr, &showManualAddressEditor_);
        ImGui::MenuItem("Show scan filters",          nullptr, &showScannerFilters_);
        ImGui::Separator();
        // The checkmark form: show the current state and let the same item
        // close the panel; also focus it when opening, so clicking a panel
        // that is already open but buried behind a dock tab brings it
        // forward rather than doing nothing visible.
        const auto panelItem = [this](const char* name, bool& flag) {
            if (ImGui::MenuItem(name, nullptr, flag)) {
                flag = true;
                focusPanel_ = name;
            }
        };
        panelItem("Memory Viewer",  showMemoryViewer_);
        panelItem("Disassembly",    showDisassembly_);
        panelItem("Breakpoints",    showBreakpoints_);
        panelItem("Access Watch",   showAccessWatch_);
        panelItem("Patches",        showPatches_);
        panelItem("Symbols",        showSymbols_);
        panelItem("Modules",        showModules_);
        panelItem("Memory Regions", showMemoryRegions_);
        panelItem("Logs",           showLogs_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        const auto panelItem = [this](const char* name, bool& flag) {
            if (ImGui::MenuItem(name, nullptr, flag)) {
                flag = true;
                focusPanel_ = name;
            }
        };
        panelItem("Pointer Scanner",  showPointerScanner_);
        panelItem("Lua Scanner",      showLuaScanner_);
        panelItem("Injection",        showInjection_);
        panelItem("Scripts",          showScripts_);
        panelItem("Structures",       showStructures_);
        panelItem("Speed and Export", showSpeed_);
        panelItem("Lua Console",      showLuaConsole_);
        panelItem("MCP Server",       showMcp_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Target")) {
        if (ImGui::MenuItem("Refresh process list")) {
            refreshProcesses();
        }
        if (ImGui::MenuItem("Refresh modules/regions", nullptr, false, services_.session().attached())) {
            services_.session().refresh();
        }
        if (ImGui::MenuItem("Detach", nullptr, false, services_.session().attached())) {
            requestDetach();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Help and safety notes")) {
            showHelp_ = true;
        }
        if (ImGui::MenuItem("About Pointer Lab")) {
            showAbout_ = true;
        }
        ImGui::Separator();
        // The crash path writes a dump automatically; this is for the other
        // half of the problem, where the application misbehaves without
        // crashing and there is otherwise nothing to send with a bug report.
        if (ImGui::MenuItem("Write a diagnostic dump")) {
            if (infra::CrashHandler::writeDumpNow()) {
                notifyInfo("Diagnostic dump written to " + infra::Paths::crashDumpFile().string());
            } else {
                notifyError("Could not write a diagnostic dump to " + infra::Paths::crashDumpFile().string());
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Saves a snapshot of Pointer Lab itself (not the target) for a bug report.");
        }
        if (ImGui::MenuItem("Open the log folder")) {
            ShellExecuteW(nullptr, L"open", infra::Paths::appData().wstring().c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Target: %s", services_.session().attached() ? domain::narrow(services_.session().processName()).c_str() : "none");
    ImGui::EndMainMenuBar();
}

void UiApp::renderCommandBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    const bool visible = ImGui::BeginViewportSideBar("##CommandBar", viewport, ImGuiDir_Up, 42.0f, flags);
    if (!visible) {
        ImGui::End();
        return;
    }

    const bool attached = services_.session().attached();
    statusPill(attached ? "ATTACHED" : "NO TARGET", attached ? colorFromBytes(30, 111, 96) : colorFromBytes(97, 77, 42));
    ImGui::SameLine();
    ImGui::TextUnformatted(attached ? domain::narrow(services_.session().processName()).c_str() : "Choose a process from the Target tab to begin.");
    if (attached) {
        ImGui::SameLine();
        ImGui::TextDisabled("PID %u", services_.session().pid());

        // Bitness is not a detail the user can afford to have to infer. It
        // decides the pointer width a chain steps by, the mode the disassembler
        // and assembler run in, and whether a DLL they built will load at all.
        const auto bitness = services_.session().bitness();
        ImGui::SameLine();
        statusPill(bitness == domain::Bitness::X86 ? "32-BIT" : "64-BIT", colorFromBytes(63, 75, 88));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(bitness == domain::Bitness::X86
                                  ? "A 32-bit (WOW64) target. Pointers are 4 bytes, the disassembler and\n"
                                    "assembler run in x86 mode, and a DLL injected here must be built x86."
                                  : "A 64-bit target. Pointers are 8 bytes, and a DLL injected here must\n"
                                    "be built x64.");
        }

        if (services_.session().readOnly()) {
            // Otherwise limited access only shows up as every individual write
            // failing for no visible reason.
            ImGui::SameLine();
            statusPill("READ-ONLY", colorFromBytes(150, 84, 40));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Only a read-only handle to this process could be obtained.\n"
                    "Scanning works, but writing, freezing, patching, breakpoints and\n"
                    "injection will all fail. Run Pointer Lab as administrator.");
            }
        }
    }

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(scaled(20.0f), 0.0f));
    ImGui::SameLine();
    const auto scanProgress = services_.scanJob().progress();
    statusPill(scanProgress.running ? "SCAN RUNNING" : "SCAN IDLE", scanProgress.running ? colorFromBytes(51, 94, 120) : colorFromBytes(50, 63, 76));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu results", scanProgress.results);

    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float pad = st.FramePadding.x * 2.0f;
        const float sp  = st.ItemSpacing.x;
        const float buttonsWidth =
            ImGui::CalcTextSize("Refresh Target").x + pad +
            ImGui::CalcTextSize("Reset Layout").x   + pad +
            ImGui::CalcTextSize("Detach").x         + pad +
            sp * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - buttonsWidth - st.WindowPadding.x);
    }
    if (ImGui::SmallButton("Refresh Target")) {
        refreshProcesses();
        if (attached) {
            services_.session().refresh();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset Layout")) {
        resetDockLayout_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!attached);
    if (ImGui::SmallButton("Detach")) {
        requestDetach();
    }
    ImGui::EndDisabled();

    ImGui::End();
}

} // namespace ire::ui
