// Panels describing the target itself: the process to attach to, and the
// modules and memory regions found inside it once attached.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

void UiApp::renderProcessPanel() {
    ImGui::Begin("Process Selection");
    statusPill(services_.session().attached() ? "CONNECTED" : "BROWSE", services_.session().attached() ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu processes", processes_.size());
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "filter by name or pid", processFilter_.data(), processFilter_.size());
    if (ImGui::Button("Refresh")) {
        refreshProcesses();
    }
    if (services_.session().attached()) {
        ImGui::SameLine();
        if (ImGui::Button("Detach")) {
            // Routed through the same path as the menu so this button cannot
            // skip the confirmation when breakpoints are still armed.
            requestDetach();
        }
    }

    if (ImGui::BeginTable("processes", 3, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, scaled(64));
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, scaled(90));
        ImGui::TableHeadersRow();
        // Lowercased once per frame rather than once per row, and matched
        // against the names refreshProcesses() already narrowed.
        std::string filterLower = processFilter_.data();
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (std::size_t row = 0; row < processes_.size(); ++row) {
            const auto& process = processes_[row];
            const auto& name = processNames_[row];
            if (!filterLower.empty()) {
                const bool matchesName = processNamesLower_[row].find(filterLower) != std::string::npos;
                const bool matchesPid = std::to_string(process.pid).find(filterLower) != std::string::npos;
                if (!matchesName && !matchesPid) {
                    continue;
                }
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", process.pid);
            ImGui::TableNextColumn();
            cellText(name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(process.pid));
            if (ImGui::Button("Attach")) {
                const auto pid = process.pid;
                const auto attach = [this, pid] {
                    if (auto result = services_.session().attach(pid); !result) {
                        notifyError("Attach failed: " + result.error());
                    }
                };
                if (services_.session().attached() && services_.session().pid() != pid) {
                    // Re-attach used to swap the process handle underneath
                    // everything else: patches, breakpoints and scripts stayed
                    // bound to the old process, so unticking a patch would
                    // write its original bytes into the new one at the same
                    // address. Route through the same detach flow the button
                    // above uses, so that class of accident cannot happen.
                    const auto oldName = domain::narrow(services_.session().processName());
                    confirmAction(
                        "Attach to a different process?",
                        "Pointer Lab is attached to " + oldName + ". Detach from it first: patches, breakpoints " +
                            "and scripts belong to that process and cannot be undone once this handle closes.",
                        "Detach and attach",
                        [this, attach] {
                            requestDetach();
                            attach();
                        });
                } else {
                    attach();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
void UiApp::renderModulesPanel() {
    ImGui::Begin("Modules", &showModules_);
    // Snapshotted once for the whole panel: session.modules() takes the lock
    // and copies every ModuleInfo, and the two wstring pairs it contains are
    // narrowed twice per row -- doing that for the count and the loop was
    // twice as much per-frame work as needed.
    const auto modules = services_.session().modules();
    ImGui::BeginDisabled(!services_.session().attached());
    if (ImGui::Button("Refresh")) {
        services_.session().refresh();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu loaded", modules.size());
    // ScrollX so the Path column can be widened past whatever width the panel
    // ended up at; the default layout docks Modules to a 20% left split, and
    // without this the Name column alone did not fit.
    if (ImGui::BeginTable("modules", 4,
                          denseTableFlags | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
                          ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Base",  ImGuiTableColumnFlags_WidthFixed,   scaled(130.0f));
        ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthFixed,    scaled(80.0f));
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthFixed,   scaled(220.0f));
        // Hidden by default because the path is long, the name is usually
        // enough, and the table already lets you tick it back on.
        ImGui::TableSetupColumn("Path",  ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 1.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(modules.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& module = modules[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::toHex(module.base).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatSize(module.size).c_str());
                ImGui::TableNextColumn();
                const auto name = domain::narrow(module.name);
                cellText(name.c_str());
                ImGui::TableNextColumn();
                const auto path = domain::narrow(module.path);
                cellText(path.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
void UiApp::renderRegionsPanel() {
    ImGui::Begin("Memory Regions", &showMemoryRegions_);
    const auto regions = services_.session().regions();
    ImGui::BeginDisabled(!services_.session().attached());
    if (ImGui::Button("Refresh")) {
        services_.session().refresh();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu regions", regions.size());
    // ScrollX plus a clipper: a real target has tens of thousands of regions
    // and both fixed-column widths and per-row layout used to be paid in full
    // every frame the panel was open.
    if (ImGui::BeginTable("regions", 6,
                          denseTableFlags | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
                          ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Base",    ImGuiTableColumnFlags_WidthFixed,   scaled(130.0f));
        ImGui::TableSetupColumn("End",     ImGuiTableColumnFlags_WidthFixed,   scaled(130.0f));
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed,    scaled(80.0f));
        ImGui::TableSetupColumn("Protect", ImGuiTableColumnFlags_WidthFixed,   scaled(200.0f));
        ImGui::TableSetupColumn("Access",  ImGuiTableColumnFlags_WidthFixed,    scaled(64.0f));
        ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthFixed,    scaled(64.0f));
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(regions.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& region = regions[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::toHex(region.base).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::toHex(region.base + region.size).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatSize(region.size).c_str());
                ImGui::TableNextColumn();
                const auto protect = platform_win32::Win32Platform::protectToString(region.protect);
                cellText(protect.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s%s%s", region.readable ? "R" : "-", region.writable ? "W" : "-",
                            region.executable ? "X" : "-");
                ImGui::TableNextColumn();
                // Truncating a 64-bit base to int made two regions whose
                // addresses differ only above bit 31 share widget state.
                ImGui::PushID(reinterpret_cast<const void*>(region.base));
                if (ImGui::SmallButton("View")) {
                    gotoMemory(region.base);
                    showMemoryViewer_ = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace ire::ui
