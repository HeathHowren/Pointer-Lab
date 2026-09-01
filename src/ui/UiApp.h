#pragma once

#include "mcp/McpServer.h"
#include "scripting/LuaConsole.h"
#include "services/RuntimeServices.h"
#include "services/UiCommands.h"
#include "storage/ProjectStore.h"
#include <Windows.h>
#include <d3d11.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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

    // --mcp: start the server as the window comes up, and open its panel so the
    // token is on screen. Starting it without showing the panel would leave the
    // one thing a client needs somewhere nobody was told to look.
    void setStartupMcpPort(std::uint16_t port) {
        startupMcpPort_ = port;
        startMcpOnLaunch_ = true;
    }

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
    // The colours and metrics on their own, scaled to dpiScale_. Split out of
    // applyStyle so a monitor change can redo it without re-adding the fonts.
    void applyStyleSizes();
    void renderDockspace();
    void buildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size);
    void renderMenu();
    void renderCommandBar();
    void renderProcessPanel();
    void renderScanPanel();
    void renderAddressListPanel();
    // Puts the manual-add / edit boxes back to their starting state, so a
    // successful Apply does not leave the previous entry's contents lying in
    // the editor for the next click to duplicate.
    void resetAddressEditor();
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
    void renderMcpPanel();
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
    // Both report through the toast channel as well as returning, because they
    // are called from the menu -- where the user is the one who needs to hear it
    // -- and from the MCP server, where the caller is. `quiet` suppresses the
    // toast for the autosave path; the returned Result carries the message
    // either way.
    infra::Result<void> saveProjectTo(const std::filesystem::path& path, bool quiet);
    infra::Result<void> loadProjectFrom(const std::filesystem::path& path, bool quiet);
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
    // resolveAddress, memoized on (text, session generation). For the address
    // boxes that resolve live on every frame: an expression naming an export
    // ("kernel32.LoadLibraryW") walks the whole export directory, which is a
    // read per named export -- fine once, a hang at sixty frames a second.
    [[nodiscard]] std::optional<std::uintptr_t> resolveAddressCached(const char* text);
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
    infra::Result<void> runOnUiThread(std::function<void()> work) override;
    infra::Result<void> saveProject(const std::string& path) override;
    infra::Result<void> loadProject(const std::string& path) override;

    struct AutomationRequest {
        enum class Kind { Screenshot, SelectPanel, SetLayout, SetWindowSize, WaitFrames, Quit, Invoke };
        Kind kind{};
        std::string text;
        int first{};
        int second{};
        // For Kind::Invoke: the caller's own code, run on the UI thread. The
        // request carries no result of its own -- whatever the work needs to
        // report, it reports through what it captured.
        std::function<void()> work;
        bool finished{};
        bool ok{true};
        std::string error;
    };

    // Queues one request and blocks until the UI thread has run it.
    infra::Result<void> automationRequest(AutomationRequest::Kind kind, std::string text = {},
                                          int first = 0, int second = 0);
    // The queue-and-wait half, shared by every request shape.
    infra::Result<void> submitRequest(std::shared_ptr<AutomationRequest> request);
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
    // Contents scale of the monitor the window is on: 1.0 at 96 DPI, 1.5 at
    // 150%. Read from the window rather than the system, because the two
    // displays on a desk are routinely set to different scales.
    float dpiScale_{1.0f};
    bool imguiInitialized_{};
    bool classRegistered_{};
    bool minimized_{};

    services::RuntimeServices services_;
    platform_win32::GlobalHotkeys hotkeys_;
    // What syncGlobalHotkeys last asked for. Compared against rather than the
    // registered set, so a key the OS refuses is not retried every frame.
    std::set<int> hotkeysRequested_;
    // The address list revision that set was computed from, so an unchanged
    // list costs a counter comparison rather than a full snapshot.
    std::uint64_t hotkeyListRevision_{};
    scripting::LuaConsole lua_;
    // Off until someone starts it from the panel. Constructed here so it drives
    // the same RuntimeServices this window does -- an agent and the person are
    // then looking at one session rather than two.
    mcp::McpServer mcpServer_;
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
    // Parallel to processes_, rebuilt by refreshProcesses(): the narrowed name
    // the table draws and the lowercased one it filters against.
    std::vector<std::string> processNames_;
    std::vector<std::string> processNamesLower_;
    std::array<char, 128> processFilter_{};

    int scanTypeIndex_{4};
    int scanModeIndex_{0};
    // 512 rather than 128: a byte pattern costs three characters per byte, so
    // 128 held only 42 pattern bytes -- and real signatures run to 60. ImGui
    // silently refused further characters, and the scan then ran against a
    // truncated pattern.
    std::array<char, 512> scanText_{};
    // The upper bound of a "value between" scan. Its own box rather than a
    // second meaning for scanText_, so switching modes never reinterprets what
    // is already typed.
    std::array<char, 512> scanText2_{};
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

    // The address list's "Current" column, formatted. Reading it inline meant
    // one ReadProcessMemory per row per frame -- for two hundred entries,
    // twelve thousand syscalls a second, every one of them taking the session
    // lock the freeze thread also wants. Refreshed on a timer instead: ten
    // times a second is faster than the eye follows, and the freeze loop next
    // door already runs at twenty for the same reason.
    std::map<std::uint64_t, std::string> currentValues_;
    std::chrono::steady_clock::time_point lastCurrentRefresh_{};

    // Same reasoning for the Patches panel: PatchRegistry::drifted reads the
    // target, and drift is a property that changes on the scale of the target
    // rewriting its own code, not of frames.
    std::map<std::uint64_t, bool> patchDrift_;
    std::chrono::steady_clock::time_point lastDriftRefresh_{};

    // And for the speed payload's status, which is four reads per frame to
    // answer a question that only changes when the slider moves.
    engine_speed::SpeedStatus speedStatus_{};
    std::chrono::steady_clock::time_point lastSpeedPoll_{};

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
    // "Resolves to" for the rows currently on screen. Walking a chain is one
    // read per level plus a module lookup, so resolving every visible row
    // every frame was thousands of syscalls a second to answer a question
    // whose answer moves on the scale of level loads.
    std::map<int, std::string> pointerResolved_;
    std::chrono::steady_clock::time_point lastPointerResolve_{};
    std::size_t pointerResolvedFor_{};

    // Backing store for resolveAddressCached. Keyed by the text; dropped
    // wholesale when the target's module table changes, which is the only
    // thing that can change what an expression resolves to.
    std::map<std::string, std::optional<std::uintptr_t>> resolveCache_;
    std::uint64_t resolveCacheGeneration_{};

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

    // Opened on demand: it is meaningless until someone wants an agent driving
    // the session, and it is not something to put in front of a first-time
    // reader by default.
    bool showMcp_{};
    int mcpPort_{8722};
    std::vector<std::string> mcpLog_;

    // Sized to match scriptSource_: a pasted script silently stopped at 4 KB
    // before, well below every other multiline editor in the app.
    std::array<char, 16384> luaInput_{};
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
    // Which thread drains the queue. A request submitted *from* that thread
    // would wait for a drain that cannot happen until it returns, so it runs
    // inline instead. Nothing does that today; it is here because the cost of
    // being wrong about it is a hung window with no message.
    std::thread::id uiThreadId_{};
    // Applied during the frame rather than at drain time: ImGui's focus call
    // belongs between NewFrame and Render.
    std::string focusPanel_;
    std::filesystem::path startupScript_;
    bool startupScriptSubmitted_{};
    bool startMcpOnLaunch_{};
    std::uint16_t startupMcpPort_{};
    bool quitRequested_{};

    // Last time the "did the target process exit under us?" check ran. Cheap
    // enough to do once a second; done every frame it would take the session
    // lock 60 times a second for no gain, since a dead process cannot come
    // back to life between ticks.
    std::chrono::steady_clock::time_point lastExitCheck_{};

    // The expanded access site's register interpretations. explain() copies the
    // module and region lists and scans them once per register, so recomputing
    // it every frame costs more than the rest of the panel together -- and the
    // answer only changes when that site is hit again.
    std::vector<engine_debug::RegisterMeaning> explainCache_;
    std::uintptr_t explainCacheSite_{};
    std::uint64_t explainCacheHits_{};
};

} // namespace ire::ui
