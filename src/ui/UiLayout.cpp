// Visual style and the docking layout: how the window looks and where the
// panels start out, as opposed to what any of them contain.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "infra/Paths.h"
#include "ui/EmbeddedFonts.h"

namespace ire::ui {

void UiApp::applyStyle() {
    ImGuiIO& io = ImGui::GetIO();

    // Fonts are compiled into the binary (see EmbeddedFonts.h). Loading them
    // from disk meant that moving the executable away from resources/fonts/
    // left monoFont_ null, and the first panel to push it dereferenced null.
    //
    // Loaded at their design size: ImGui rasterizes at base size times
    // style.FontScaleDpi, which it keeps in step with the monitor itself once
    // io.ConfigDpiScaleFonts is on. Loading them pre-multiplied would fight it.
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(RobotoMedium_compressed_data_base85, 15.0f);
    monoFont_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(CousineRegular_compressed_data_base85, 14.0f);
    if (!monoFont_) {
        // Embedded data can only fail to build under memory exhaustion; fall
        // back to the always-present default font rather than a null pointer.
        monoFont_ = io.Fonts->AddFontDefault();
    }

    applyStyleSizes();
}

// Rebuilt from the unscaled values every time rather than scaled in place: at
// 125% a padding of 5 truncates to 6, and scaling that again on the way back
// down does not return 5. Two trips across a DPI boundary and the layout has
// visibly drifted.
void UiApp::applyStyleSizes() {
    ImGuiIO& io = ImGui::GetIO();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(12.0f, 10.0f);
    style.FramePadding      = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 5.0f);
    style.WindowRounding    = 9.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 7.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.IndentSpacing     = 14.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 8.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg]       = colorFromBytes(18, 22, 27);
    colors[ImGuiCol_ChildBg]        = colorFromBytes(22, 27, 33);
    colors[ImGuiCol_PopupBg]        = colorFromBytes(25, 30, 37);
    colors[ImGuiCol_Border]         = colorFromBytes(55, 65, 75);
    colors[ImGuiCol_FrameBg]        = colorFromBytes(31, 38, 47);
    colors[ImGuiCol_FrameBgHovered] = colorFromBytes(41, 52, 63);
    colors[ImGuiCol_FrameBgActive]  = colorFromBytes(50, 63, 76);
    colors[ImGuiCol_TitleBg]        = colorFromBytes(15, 19, 24);
    colors[ImGuiCol_TitleBgActive]  = colorFromBytes(28, 36, 44);
    colors[ImGuiCol_MenuBarBg]      = colorFromBytes(13, 17, 21);
    colors[ImGuiCol_Button]         = colorFromBytes(39, 57, 68);
    colors[ImGuiCol_ButtonHovered]  = colorFromBytes(54, 79, 94);
    colors[ImGuiCol_ButtonActive]   = colorFromBytes(69, 98, 113);
    colors[ImGuiCol_Header]         = colorFromBytes(35, 51, 61);
    colors[ImGuiCol_HeaderHovered]  = colorFromBytes(50, 73, 86);
    colors[ImGuiCol_HeaderActive]   = colorFromBytes(64, 92, 105);
    colors[ImGuiCol_Tab]            = colorFromBytes(24, 30, 37);
    colors[ImGuiCol_TabHovered]     = colorFromBytes(61, 91, 104);
    colors[ImGuiCol_TabSelected]    = colorFromBytes(39, 56, 66);
    colors[ImGuiCol_TabDimmed]      = colorFromBytes(18, 23, 28);
    colors[ImGuiCol_TabDimmedSelected]  = colorFromBytes(30, 40, 49);
    colors[ImGuiCol_DockingPreview]     = colorFromBytes(92, 166, 184, 180);
    colors[ImGuiCol_CheckMark]          = colorFromBytes(114, 211, 189);
    colors[ImGuiCol_SliderGrab]         = colorFromBytes(101, 177, 194);
    colors[ImGuiCol_SliderGrabActive]   = colorFromBytes(126, 214, 225);
    colors[ImGuiCol_ResizeGrip]         = colorFromBytes(61, 91, 104, 90);
    colors[ImGuiCol_ResizeGripHovered]  = colorFromBytes(92, 166, 184, 160);
    colors[ImGuiCol_Text]               = colorFromBytes(227, 233, 238);
    colors[ImGuiCol_TextDisabled]       = colorFromBytes(137, 149, 158);

    colors[ImGuiCol_ScrollbarBg]          = colorFromBytes(14, 18, 22);
    colors[ImGuiCol_ScrollbarGrab]        = colorFromBytes(45, 57, 70);
    colors[ImGuiCol_ScrollbarGrabHovered] = colorFromBytes(60, 75, 90);
    colors[ImGuiCol_ScrollbarGrabActive]  = colorFromBytes(80, 100, 115);

    colors[ImGuiCol_TableHeaderBg]    = colorFromBytes(28, 36, 44);
    colors[ImGuiCol_TableBorderStrong]= colorFromBytes(55, 65, 75);
    colors[ImGuiCol_TableBorderLight] = colorFromBytes(35, 43, 53);
    colors[ImGuiCol_TableRowBg]       = colorFromBytes(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]    = colorFromBytes(255, 255, 255, 8);

    colors[ImGuiCol_Separator]        = colorFromBytes(55, 65, 75);
    colors[ImGuiCol_SeparatorHovered] = colorFromBytes(92, 166, 184);
    colors[ImGuiCol_SeparatorActive]  = colorFromBytes(126, 214, 225);

    colors[ImGuiCol_NavCursor]        = colorFromBytes(101, 177, 194, 200);
    colors[ImGuiCol_ModalWindowDimBg] = colorFromBytes(0, 0, 0, 120);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Padding, rounding and scrollbar widths are pixels, and ImGui scales none
    // of them for DPI on its own -- only fonts and window rectangles. Without
    // this, text on a 150% display grows and the boxes around it do not.
    style.ScaleAllSizes(dpiScale_);
}

void UiApp::renderDockspace() {
    const ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(ImGui::GetID("PointerLabDockspace"), viewport, dockspaceFlags);

    const bool needsFirstLayout = !dockLayoutInitialized_ && !std::filesystem::exists(infra::Paths::layoutFile());
    if (resetDockLayout_ || needsFirstLayout) {
        // Reset Layout has to reopen closed panels too. Restoring the default
        // arrangement while leaving half the windows hidden is not the default
        // arrangement, and it leaves the user with no way back.
        showMemoryViewer_ = showDisassembly_ = showBreakpoints_ = true;
        showModules_ = showMemoryRegions_ = showLogs_ = true;
        showPointerScanner_ = showLuaScanner_ = showInjection_ = showLuaConsole_ = true;
        buildDefaultDockLayout(dockspaceId, viewport->WorkSize);
        resetDockLayout_ = false;
    }
    dockLayoutInitialized_ = true;
}

void UiApp::buildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);

    // Each split carves a slice off the shrinking main node and hands back the
    // slice. Splitting a previously returned id instead silently collapses
    // later splits into the same node, which is how every panel past the third
    // ended up in one tab bar.
    ImGuiID main = dockspaceId;
    const ImGuiID left         = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left,  0.20f, nullptr, &main);
    const ImGuiID right        = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.28f, nullptr, &main);
    ImGuiID bottomLeft         = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down,  0.30f, nullptr, &main);
    const ImGuiID centerBottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down,  0.42f, nullptr, &main);
    const ImGuiID bottomRight  = ImGui::DockBuilderSplitNode(bottomLeft, ImGuiDir_Right, 0.50f, nullptr, &bottomLeft);
    const ImGuiID centerTop    = main;

    // Every panel gets a home. Docking only the first three left a first-run
    // user looking at a third of the tool with no indication the rest existed;
    // the View menu is not somewhere people look before deciding a feature is
    // missing.
    ImGui::DockBuilderDockWindow("Process Selection", left);
    ImGui::DockBuilderDockWindow("Modules",           left);
    ImGui::DockBuilderDockWindow("Memory Regions",    left);

    ImGui::DockBuilderDockWindow("Scanner",           centerTop);
    ImGui::DockBuilderDockWindow("Address List",      centerBottom);

    // The panels that open on demand rather than on start. They still need a
    // home, or the first time one is opened it appears as a small floating
    // window over the middle of everything. They share the Address List's tab
    // bar, which stays uncluttered because a closed panel has no tab.
    ImGui::DockBuilderDockWindow("Access Watch",      centerBottom);
    ImGui::DockBuilderDockWindow("Patches",           centerBottom);
    ImGui::DockBuilderDockWindow("Symbols",           centerBottom);
    ImGui::DockBuilderDockWindow("Scripts",           centerBottom);
    ImGui::DockBuilderDockWindow("Structures",        centerBottom);
    ImGui::DockBuilderDockWindow("Speed and Export",  centerBottom);

    ImGui::DockBuilderDockWindow("Memory Viewer",     right);
    ImGui::DockBuilderDockWindow("Disassembly",       right);
    ImGui::DockBuilderDockWindow("Breakpoints",       right);

    ImGui::DockBuilderDockWindow("Pointer Scanner",   bottomLeft);
    ImGui::DockBuilderDockWindow("Lua Scanner",       bottomLeft);

    ImGui::DockBuilderDockWindow("Injection",         bottomRight);
    ImGui::DockBuilderDockWindow("Lua Console",       bottomRight);
    ImGui::DockBuilderDockWindow("MCP Server",        bottomRight);
    ImGui::DockBuilderDockWindow("Logs",              bottomRight);

    ImGui::DockBuilderFinish(dockspaceId);
    infra::Logger::instance().trace(
        "Layout nodes: size=" + std::to_string(static_cast<int>(size.x)) + "x" +
        std::to_string(static_cast<int>(size.y)) + " left=" + std::to_string(left) + " right=" +
        std::to_string(right) + " centerTop=" + std::to_string(centerTop) + " centerBottom=" +
        std::to_string(centerBottom) + " bottomLeft=" + std::to_string(bottomLeft) + " bottomRight=" +
        std::to_string(bottomRight));
    infra::Logger::instance().info("Applied default layout.");
}

} // namespace ire::ui
