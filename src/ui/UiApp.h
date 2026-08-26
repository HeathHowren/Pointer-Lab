#pragma once

#include "scripting/LuaConsole.h"
#include "services/RuntimeServices.h"
#include "services/UiCommands.h"
#include "storage/ProjectStore.h"
#include <Windows.h>
#include <d3d11.h>
#include <imgui.h>

#include <array>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ire::ui {

class UiApp final : public services::UiCommands {
public:
    UiApp(HINSTANCE instance, int showCommand);
    ~UiApp() override;

    UiApp(const UiApp&) = delete;
    UiApp& operator=(const UiApp&) = delete;

    int run();

    // Lua to run once the window is up, from --script. Empty for an ordinary
    // interactive start.
    void setStartupScript(std::filesystem::path path) { startupScript_ = std::move(path); }

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
    // Points the hex editor at an address and shows it. Sets the cursor as well
    // as the address box, so following a pointer moves the view rather than
    // only rewriting text the user then has to press Go on.
    void gotoMemory(std::uintptr_t address);
    void renderDisassemblyPanel();
    void renderBreakpointPanel();
    void renderAccessWatchPanel();
    void renderAccessWatchDetail(const engine_debug::AccessSite& site);
    // Starts a watch and opens the panel. Called from the address list and the
    // scan results, which is where the question is actually asked.
    void beginAccessWatch(std::uintptr_t address, domain::ValueType type, bool writesOnly);
    // Replaces one instruction with nops, behind the usual confirmation. Length
    // comes from the decoded instruction, so the patch covers it exactly and
    // never leaves a truncated instruction behind.
    void confirmNopInstruction(std::uintptr_t address, std::size_t length, const std::string& text);
    void renderPatchesPanel();
    void renderSymbolsPanel();
    void renderScriptsPanel();
    // Starts a script from one of the templates, filled in with a real address,
    // the bytes there, and the module they belong to. templateShape is an
    // engine_aa::AutoAssembler::Template; it is passed as an int so the panels
    // that call this do not all have to include the auto-assembler.
    void newScriptFromAddress(std::uintptr_t address, const std::vector<std::uint8_t>& bytes,
                              int templateShape);
    void renderStructuresPanel();
    // The inline editor for one field: name, type, and the length that only the
    // variable-width types have.
    void renderStructureFieldEditor();
    void renderStructureFieldMenu(const domain::StructureField& field,
                                  const std::vector<std::uintptr_t>& addresses);
    // Adds an address to the structure view and opens it, creating a structure
    // if there is not one yet. Called from every panel that has an address in
    // its hand: the address list, the scan results, the hex editor.
    void dissect(std::uintptr_t address);
    // The address box parsed into resolved addresses. Anything the other address
    // boxes accept works here, so a structure can be pinned to `game.exe+0x…`
    // and keep working after a restart.
    [[nodiscard]] std::vector<std::uintptr_t> structureAddressList();
    void renderSpeedPanel();
    // Writes the address list out as a compilable trainer project. Not an
    // executable: see engine_export/TrainerExport.h for why.
    void exportTrainer();
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
    // parseAddress, plus every name the symbol table knows: user symbols,
    // `client.dll+0x4A2C10`, `kernel32.LoadLibraryW`. Every address box in the
    // tool goes through this, so an expression written down once keeps working
    // after the target restarts and ASLR moves the module.
    [[nodiscard]] std::optional<std::uintptr_t> resolveAddress(const char* text);
    // The same, but reporting why it failed. For the places where a wrong
    // answer is expensive enough to be worth a sentence.
    [[nodiscard]] infra::Result<std::uintptr_t> resolveAddressOrExplain(const char* text);
    static void copyText(char* destination, std::size_t size, const std::string& text);

    // ---------------------------------------------------------------------
    // services::UiCommands -- called from the Lua worker thread
    //
    // Private overrides of public virtuals: nothing but a UiCommands* should
    // reach these, because the marshalling onto the UI thread is the whole
    // point and a direct call from this class would skip it.
    // ---------------------------------------------------------------------
    infra::Result<void> screenshot(const std::string& path) override;
    infra::Result<void> selectPanel(const std::string& name) override;
    infra::Result<void> setLayout(const std::string& name) override;
    infra::Result<void> setWindowSize(int width, int height) override;
    infra::Result<void> waitFrames(int frames) override;
    infra::Result<void> quit() override;

    struct AutomationRequest {
        enum class Kind { Screenshot, SelectPanel, SetLayout, SetWindowSize, WaitFrames, Quit };
        Kind kind{};
        std::string text;
        int first{};
        int second{};
        bool finished{};
        bool ok{true};
        std::string error;
    };

    // Queues one request and blocks until the UI thread has run it.
    infra::Result<void> automationRequest(AutomationRequest::Kind kind, std::string text = {},
                                          int first = 0, int second = 0);
    // Everything except a screenshot, run before the frame is built so that
    // opening a panel affects the frame about to be drawn.
    void drainAutomation();
    // Screenshots, run after the draw and before Present. The swap chain
    // discards on present, so after it there is nothing left to capture.
    void drainScreenshots();
    infra::Result<void> captureBackBuffer(const std::string& path);
    // The show flag behind a panel title, or null for the three that are always
    // open and for a name that is not a panel.
    bool* panelFlag(const std::string& name);

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
    // Opened on demand rather than by default: it is meaningless until a watch
    // is running, and it wants the room when it is.
    bool showAccessWatch_{};
    // Same: empty until something has been patched, and opened by the action
    // that fills it so the undo is where the user is already looking.
    bool showPatches_{};
    bool showSymbols_{};
    std::array<char, 96> symbolName_{};
    std::array<char, 128> symbolExpression_{};
    bool showScripts_{};
    std::uint64_t editScriptId_{};
    std::array<char, 128> scriptName_{};
    std::array<char, 16384> scriptSource_{};
    bool showStructures_{};
    std::uint64_t structureId_{};
    std::array<char, 128> structureName_{};
    std::array<char, 256> structureAddresses_{};
    int structureSize_{0x100};
    // Which field the inline editor is on, if any. An optional rather than a
    // sentinel offset, because +0 is a perfectly ordinary field to be editing.
    std::optional<std::ptrdiff_t> editFieldOffset_;
    std::array<char, 96> editFieldName_{};
    int editFieldTypeIndex_{4};
    int editFieldLength_{4};
    bool showSpeed_{};
    float speedScale_{1.0f};
    std::array<char, 96> trainerName_{};
    std::array<char, 512> trainerDirectory_{};
    // Trap address of the site whose registers are expanded, or 0 for none.
    std::uintptr_t accessWatchDetail_{};

    std::vector<domain::ProcessInfo> processes_;
    std::array<char, 128> processFilter_{};

    int scanTypeIndex_{4};
    int scanModeIndex_{0};
    std::array<char, 128> scanText_{};
    // The upper bound of a "value between" scan. Its own box rather than a
    // second meaning for scanText_, so switching modes never reinterprets what
    // is already typed.
    std::array<char, 128> scanText2_{};
    bool scanCaseInsensitive_{};
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

    // Manual pointer entry. With this ticked, the Address box holds the *base*
    // of a chain rather than the value's own address, and the offsets below say
    // how to walk from it. This is how a chain read out of the access watch or
    // followed by hand in the hex editor gets written down.
    bool addAsPointer_{};
    std::array<char, 192> addPointerOffsets_{};

    // Per-row write popup, so a row's Write button no longer takes its value
    // from the shared manual-editor field.
    std::array<char, 128> rowWriteValue_{};
    std::uint64_t rowWriteId_{};

    std::array<char, 96> memoryAddress_{};
    std::array<char, 256> memoryPatch_{};
    int memoryReadSize_{256};
    // Where the hex editor is actually looking. Distinct from memoryAddress_,
    // which is what the user typed: scrolling and following a pointer move the
    // view without rewriting the box under their cursor.
    std::uintptr_t memoryCursor_{};
    // Byte being edited in place, as an offset from memoryCursor_, or -1.
    int memoryEditOffset_{-1};
    std::array<char, 8> memoryEditText_{};
    // The previous frame's window, so bytes that changed can be highlighted.
    // A game writing to a structure while you watch is the fastest way to see
    // which part of it is the value you are after.
    std::uintptr_t memoryPreviousBase_{};
    std::vector<std::uint8_t> memoryPrevious_;

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

    // Scripted capture. The queue is drained on the UI thread; the script
    // thread waits on the condition variable for its own request to be marked
    // finished.
    std::mutex automationMutex_;
    std::condition_variable automationDone_;
    std::vector<std::shared_ptr<AutomationRequest>> automationQueue_;
    // Applied during the frame rather than at drain time: ImGui's focus call
    // belongs between NewFrame and Render.
    std::string focusPanel_;
    std::filesystem::path startupScript_;
    bool startupScriptSubmitted_{};
    bool quitRequested_{};
};

} // namespace ire::ui
