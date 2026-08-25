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
    ImGui::InputText("##filter", processFilter_.data(), processFilter_.size());
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
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        for (const auto& process : processes_) {
            if (!textMatchesFilter(process.name, processFilter_.data())) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", process.pid);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::narrow(process.name).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(process.pid));
            if (ImGui::Button("Attach")) {
                if (auto result = services_.session().attach(process.pid); !result) {
                    notifyError("Attach failed: " + result.error());
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
    if (ImGui::Button("Refresh") && services_.session().attached()) {
        services_.session().refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu loaded", services_.session().modules().size());
    if (ImGui::BeginTable("modules", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Base",  ImGuiTableColumnFlags_WidthFixed,   130.0f);
        ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthFixed,    80.0f);
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthFixed,   160.0f);
        ImGui::TableSetupColumn("Path",  ImGuiTableColumnFlags_WidthStretch,   1.0f);
        ImGui::TableHeadersRow();
        for (const auto& module : services_.session().modules()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(module.base).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatSize(module.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::narrow(module.name).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::narrow(module.path).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
void UiApp::renderRegionsPanel() {
    ImGui::Begin("Memory Regions", &showMemoryRegions_);
    if (ImGui::Button("Refresh") && services_.session().attached()) {
        services_.session().refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu regions", services_.session().regions().size());
    if (ImGui::BeginTable("regions", 6, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Base",    ImGuiTableColumnFlags_WidthFixed,   130.0f);
        ImGui::TableSetupColumn("End",     ImGuiTableColumnFlags_WidthFixed,   130.0f);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed,    80.0f);
        ImGui::TableSetupColumn("Protect", ImGuiTableColumnFlags_WidthFixed,    160.0f);
        ImGui::TableSetupColumn("Access",  ImGuiTableColumnFlags_WidthFixed,    48.0f);
        ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthFixed,    48.0f);
        ImGui::TableHeadersRow();
        for (const auto& region : services_.session().regions()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(region.base).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(region.base + region.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatSize(region.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(platform_win32::Win32Platform::protectToString(region.protect).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s%s%s", region.readable ? "R" : "-", region.writable ? "W" : "-", region.executable ? "X" : "-");
            ImGui::TableNextColumn();
            // Truncating a 64-bit base to int made two regions whose addresses
            // differ only above bit 31 share widget state.
            ImGui::PushID(reinterpret_cast<const void*>(region.base));
            if (ImGui::SmallButton("View")) {
                copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(region.base));
                showMemoryViewer_ = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace ire::ui
