#pragma once

#include "scripting/LuaConsole.h"
#include "services/RuntimeServices.h"
#include "storage/ProjectStore.h"
#include <Windows.h>
#include <d3d11.h>
#include <imgui.h>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
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
    static void reportFatalStartupError(const wchar_t* message);
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
    void renderToasts();
    void renderConfirmModal();
    void renderAboutWindow();
    void renderHelpWindow();
    void handleHotkeys();
    // Registers exactly the F-keys currently assigned to an address list entry,
    // so Pointer Lab claims no key it is not actually using.
    void syncGlobalHotkeys();

    // Failures used to reach the user only through the Logs panel, which is
    // hidden by default, so a failed patch looked exactly like a successful
    // one. Everything user-facing goes through these.
    void notifyInfo(const std::string& text);
    void notifyError(const std::string& text);
    void requestDetach();

    // Project persistence. ProjectStore existed and worked but nothing ever
    // called it, so the address list was lost on every exit.
    void newProject();
    void openProjectDialog();
    void saveProject();
    void saveProjectAs();
    bool saveProjectTo(const std::filesystem::path& path, bool quiet);
    bool loadProjectFrom(const std::filesystem::path& path, bool quiet);
    void loadSession();
    void saveSession();
    // Preferences that belong to the installation rather than to a project.
    void loadSettings();
    void saveSettings();
    [[nodiscard]] std::string projectTitle() const;

    // Queues a confirmation dialog; action runs only if the user accepts.
    void confirmAction(std::string title, std::string message, std::string confirmLabel, std::function<void()> action);

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
    bool imguiInitialized_{};
    bool classRegistered_{};
    bool minimized_{};

    services::RuntimeServices services_;
    platform_win32::GlobalHotkeys hotkeys_;
    // What syncGlobalHotkeys last asked for. Compared against rather than the
    // registered set, so a key the OS refuses is not retried every frame.
    std::set<int> hotkeysRequested_;
    scripting::LuaConsole lua_;
    storage::ProjectStore projectStore_;
    std::filesystem::path projectPath_;

    bool dockLayoutInitialized_{};
    bool resetDockLayout_{};
    bool showManualAddressEditor_{};
    bool showScannerFilters_{};
    // Panels are open by default and have a home in the default layout. Hiding
    // them until the user finds the View menu made most of the tool invisible
    // on a first run; a panel that is in the way can be closed, but one that
    // was never seen cannot be looked for.
    bool showMemoryViewer_{true};
    bool showDisassembly_{true};
    bool showBreakpoints_{true};
    bool showModules_{true};
    bool showMemoryRegions_{true};
    bool showLogs_{true};
    bool showPointerScanner_{true};
    bool showLuaScanner_{true};
    bool showInjection_{true};
    bool showLuaConsole_{true};

    std::vector<domain::ProcessInfo> processes_;
    std::array<char, 128> processFilter_{};

    int scanTypeIndex_{4};
    int scanModeIndex_{0};
    std::array<char, 128> scanText_{};
    bool scanWritableOnly_{};
    bool scanExecutableOnly_{};
    int scanMaxResults_{1000000};
    float scanFloatEpsilon_{0.0001f};

    std::array<char, 64> addAddress_{};
    std::array<char, 128> addDescription_{};
    std::array<char, 128> addGroup_{};
    std::array<char, 128> addValue_{};
    std::array<char, 32> addHotkey_{};
    int addTypeIndex_{4};
    std::uint64_t editEntryId_{};

    // Per-row write popup, so a row's Write button no longer takes its value
    // from the shared manual-editor field.
    std::array<char, 128> rowWriteValue_{};
    std::uint64_t rowWriteId_{};

    std::array<char, 64> memoryAddress_{};
    std::array<char, 256> memoryPatch_{};
    int memoryReadSize_{256};

    std::array<char, 64> disasmAddress_{};
    std::array<char, 2048> assemblerText_{};

    std::array<char, 64> breakpointAddress_{};
    std::array<char, 128> breakpointLabel_{};
    int breakpointKindIndex_{};
    // Index into {1, 2, 4, 8}; 4 bytes is the common case.
    int breakpointWidthIndex_{2};

    std::array<char, 64> pointerTarget_{};
    int pointerDepth_{3};
    int pointerTypeIndex_{4};
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

    std::array<char, 128> logFilter_{};

    struct Toast {
        std::string text;
        bool error{};
        float secondsRemaining{};
    };
    std::vector<Toast> toasts_;

    struct PendingConfirm {
        std::string title;
        std::string message;
        std::string confirmLabel;
        std::function<void()> action;
        bool opened{};
    };
    std::optional<PendingConfirm> pendingConfirm_;

    bool showAbout_{};
    bool showHelp_{};
};

} // namespace ire::ui
