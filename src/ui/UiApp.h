#pragma once

#include "scripting/LuaConsole.h"
#include "services/RuntimeServices.h"
#include <Windows.h>
#include <d3d11.h>
#include <imgui.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ire::ui {

class UiApp {
public:
    UiApp(HINSTANCE instance, int showCommand);
    ~UiApp();

    UiApp(const UiApp&) = delete;
    UiApp& operator=(const UiApp&) = delete;

    int run();

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool createWindow();
    bool createDeviceD3D(HWND hwnd);
    void cleanupDeviceD3D();
    void createRenderTarget();
    void cleanupRenderTarget();
    void render();
    void applyStyle();
    void renderDockspace();
    void buildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size);
    void renderMenu();
    void renderCommandBar();
    void renderProcessPanel();
    void renderScanPanel();
    void renderAddressListPanel();
    void renderMemoryPanel();
    void renderDisassemblyPanel();
    void renderBreakpointPanel();
    void renderModulesPanel();
    void renderRegionsPanel();
    void renderPointerPanel();
    void renderInjectionPanel();
    void renderLuaScannerPanel();
    void renderLuaPanel();
    void renderLogPanel();
    void handleHotkeys();

    void refreshProcesses();

    static std::optional<std::uintptr_t> parseAddress(const char* text);
    static void copyText(char* destination, std::size_t size, const std::string& text);

    HINSTANCE instance_{};
    int showCommand_{};
    HWND hwnd_{};
    ID3D11Device* device_{};
    ID3D11DeviceContext* deviceContext_{};
    IDXGISwapChain* swapChain_{};
    ID3D11RenderTargetView* renderTargetView_{};
    ImFont* monoFont_{};

    services::RuntimeServices services_;
    scripting::LuaConsole lua_;

    bool dockLayoutInitialized_{};
    bool resetDockLayout_{};
    ImGuiID snapLeftId_{};
    ImGuiID snapCenterTopId_{};
    ImGuiID snapCenterBottomId_{};
    bool showManualAddressEditor_{};
    bool showScannerFilters_{};
    bool showMemoryViewer_{};
    bool showDisassembly_{};
    bool showBreakpoints_{};
    bool showModules_{};
    bool showMemoryRegions_{};
    bool showLogs_{};
    bool showPointerScanner_{};
    bool showLuaScanner_{};
    bool showInjection_{};
    bool showLuaConsole_{};

    std::vector<domain::ProcessInfo> processes_;
    std::array<char, 128> processFilter_{};

    int scanTypeIndex_{4};
    int scanModeIndex_{0};
    std::array<char, 128> scanText_{};
    bool scanWritableOnly_{};
    bool scanExecutableOnly_{};

    std::array<char, 64> addAddress_{};
    std::array<char, 128> addDescription_{};
    std::array<char, 128> addGroup_{};
    std::array<char, 128> addValue_{};
    std::array<char, 32> addHotkey_{};
    int addTypeIndex_{4};
    std::uint64_t editEntryId_{};

    std::array<char, 64> memoryAddress_{};
    std::array<char, 256> memoryPatch_{};
    int memoryReadSize_{256};

    std::array<char, 64> disasmAddress_{};
    std::array<char, 2048> assemblerText_{};

    std::array<char, 64> breakpointAddress_{};
    std::array<char, 128> breakpointLabel_{};

    std::array<char, 64> pointerTarget_{};
    int pointerDepth_{3};
    std::array<char, 64> pointerMaxOffset_{"0x1000"};

    std::array<char, 64> allocSize_{"4096"};
    std::array<char, 64> threadStart_{};
    std::array<char, 64> threadParameter_{"0"};
    std::array<char, 512> dllPath_{};

    int luaScanTypeIndex_{4};
    int luaScanStride_{0};
    int luaScanMaxResults_{50000};
    bool luaScanWritableOnly_{};
    bool luaScanExecutableOnly_{};
    std::array<char, 8192> luaScanScript_{};

    std::array<char, 4096> luaInput_{};
    std::vector<std::string> luaOutput_;
};

} // namespace ire::ui
