#include "ui/UiApp.h"

#include "infra/CrashHandler.h"
#include "infra/Logger.h"
#include "infra/Paths.h"
#include "infra/Settings.h"
#include "platform_win32/Win32Platform.h"
#include "ui/EmbeddedFonts.h"

#include <Version.h>

#include <commdlg.h>
#include <shellapi.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include "ui/UiInternal.h"

namespace ire::ui {

UiApp::UiApp(HINSTANCE instance, int showCommand)
    : instance_(instance), showCommand_(showCommand), lua_(services_), mcpServer_(services_) {
    // The thread that will drain the automation queue, recorded before anything
    // can submit to it. A request made from this thread would wait for a drain
    // that cannot run until it returns.
    uiThreadId_ = std::this_thread::get_id();
    copyText(addDescription_.data(), addDescription_.size(), "Manual entry");
    copyText(addGroup_.data(), addGroup_.size(), "Default");
    copyText(luaScanScript_.data(), luaScanScript_.size(),
        "return function(ctx)\n"
        "    -- ctx.address, ctx.value, ctx.bytes, ctx.hex, ctx.type\n"
        "    return ctx.value ~= nil and ctx.value > 1000\n"
        "end\n");
    // Registered here rather than in run(), so a script submitted before the
    // first frame still finds a window to talk to.
    services::setUiCommands(this);
}

UiApp::~UiApp() {
    // First of all, and before the seam below is cleared: a tool call in flight
    // would otherwise find uiCommands() null, decide there is no window to
    // marshal onto, and run itself inline against engines that are being
    // destroyed. Stopping the server joins its thread, so by the time this
    // returns there is no call in flight to worry about.
    mcpServer_.stop();
    // Before anything is torn down: a script thread blocked on a request must
    // not find a half-destroyed window on the other end of it.
    services::setUiCommands(nullptr);
    // run() can bail before ImGui is initialised (window or D3D creation
    // failure); shutting the backends down in that case is undefined behaviour.
    if (imguiInitialized_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    cleanupDeviceD3D();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (classRegistered_) {
        UnregisterClassW(L"PointerLabWindow", instance_);
    }
}

int UiApp::run() {
    if (!createWindow()) {
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiInitialized_ = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    static std::string iniPath = infra::Paths::layoutFile().string();
    io.IniFilename = iniPath.c_str();

    // resources/app.manifest has declared per-monitor-v2 awareness since 1.0,
    // which means Windows hands us real pixels and expects us to do the
    // scaling. Until now nothing did, so every control was laid out at 96 DPI
    // and the whole window came out roughly two thirds size on a 150% display.
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    dpiScale_ = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_);

    applyStyle();

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_, deviceContext_);

    refreshProcesses();
    loadSettings();
    loadSession();

    ShowWindow(hwnd_, showCommand_);
    UpdateWindow(hwnd_);

    bool done = false;
    while (!done) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }
        if (minimized_) {
            // Nothing is visible; yield instead of rendering at full rate.
            Sleep(16);
            continue;
        }
        render();

        // --mcp, once there is a window to show the token in. Cleared whether or
        // not it worked, so a refused port is reported once rather than retried
        // every frame.
        if (startMcpOnLaunch_) {
            startMcpOnLaunch_ = false;
            showMcp_ = true;
            focusPanel_ = "MCP Server";
            if (auto started = mcpServer_.start(startupMcpPort_); !started) {
                notifyError(started.error());
            } else {
                mcpPort_ = static_cast<int>(mcpServer_.port());
                notifyInfo("MCP server listening on " + mcpServer_.url() +
                           ". The token is in the MCP Server panel.");
            }
        }

        // --script, submitted after the first frame rather than before it: the
        // script's very first act is usually to size the window and reset the
        // layout, and neither means anything until there is something to size.
        if (!startupScript_.empty() && !startupScriptSubmitted_) {
            startupScriptSubmitted_ = true;
            std::ifstream file(startupScript_, std::ios::binary);
            if (!file) {
                notifyError("Could not read the script " + startupScript_.string() + ".");
            } else {
                std::ostringstream contents;
                contents << file.rdbuf();
                infra::Logger::instance().info("Running " + startupScript_.string() + ".");
                if (!lua_.submit(contents.str())) {
                    notifyError("A script is already running.");
                }
            }
        }

        // quit() from a script. Posted rather than acted on directly so the
        // frame finishes and the usual autosave path runs.
        if (quitRequested_) {
            quitRequested_ = false;
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }

    // Autosave so the address list survives a normal exit even when the user
    // never explicitly saved a project.
    saveSession();
    saveSettings();
    return 0;
}

LRESULT CALLBACK UiApp::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }

    UiApp* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<UiApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<UiApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->handleMessage(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT UiApp::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        minimized_ = (wParam == SIZE_MINIMIZED);
        if (device_ != nullptr && !minimized_) {
            cleanupRenderTarget();
            const HRESULT resized = swapChain_->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(resized)) {
                infra::Logger::instance().error("Swap chain resize failed.");
            }
            // Rebuilt unconditionally: render() also repairs a null view, so a
            // failed resize degrades to a skipped frame rather than a crash.
            createRenderTarget();
        }
        return 0;
    case WM_DPICHANGED:
        // Dragged to a display with a different scale. The ImGui backend has
        // already moved the window to the rect Windows suggested and will
        // rescale the docked layout; what it does not touch is the style
        // metrics, so those are rebuilt here at the new scale.
        if (imguiInitialized_) {
            dpiScale_ = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
            applyStyleSizes();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_HOTKEY: {
        // The point of the whole feature: this arrives while the *target* is in
        // the foreground, which is when a freeze toggle is actually wanted.
        const int id = static_cast<int>(wParam);
        if (id >= platform_win32::GlobalHotkeys::firstId && id <= platform_win32::GlobalHotkeys::lastId) {
            services_.addressList().toggleHotkey("F" + std::to_string(id));
        }
        return 0;
    }
    case WM_DESTROY:
        // Before the window goes away: an unregistered hotkey needs the handle
        // it was registered against.
        hotkeys_.unregisterAll();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool UiApp::createWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = &UiApp::windowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"PointerLabWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Icon 1 comes from resources/PointerLab.rc.
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        reportFatalStartupError(L"Could not register the Pointer Lab window class.");
        return false;
    }
    classRegistered_ = true;

    hwnd_ = CreateWindowW(wc.lpszClassName, L"Pointer Lab", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1500, 950, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        reportFatalStartupError(L"Could not create the Pointer Lab main window.");
        return false;
    }

    // 1500x950 is a size in ordinary pixels, which on a 150% display is a
    // window two thirds the intended size holding text drawn at full size.
    // Grown to match, but never past the monitor's work area.
    if (const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_); scale > 1.0f) {
        auto width = static_cast<LONG>(1500.0f * scale);
        auto height = static_cast<LONG>(950.0f * scale);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            width = std::min(width, monitor.rcWork.right - monitor.rcWork.left);
            height = std::min(height, monitor.rcWork.bottom - monitor.rcWork.top);
        }
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }

    if (!createDeviceD3D(hwnd_)) {
        cleanupDeviceD3D();
        reportFatalStartupError(
            L"Could not initialise Direct3D 11.\n\n"
            L"Pointer Lab needs a Direct3D 11 capable display adapter. "
            L"If you are running inside a virtual machine or over a remote "
            L"session, try enabling graphics acceleration.");
        return false;
    }
    return true;
}

void UiApp::reportFatalStartupError(const wchar_t* message) {
    // This is a WIN32-subsystem executable with no console, so a failure before
    // the window exists would otherwise be completely silent - the user
    // double-clicks and nothing happens at all.
    infra::Logger::instance().error(domain::narrow(message));
    MessageBoxW(nullptr, message, L"Pointer Lab", MB_ICONERROR | MB_OK);
}

bool UiApp::createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &featureLevel, &deviceContext_);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &featureLevel, &deviceContext_);
    }
    if (FAILED(result)) {
        return false;
    }
    createRenderTarget();
    return true;
}

void UiApp::cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (deviceContext_) { deviceContext_->Release(); deviceContext_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

void UiApp::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    const HRESULT gotBuffer = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(gotBuffer) || !backBuffer) {
        // Leaving renderTargetView_ null here used to feed a null view straight
        // into OMSetRenderTargets/ClearRenderTargetView on the next frame.
        infra::Logger::instance().error("Could not obtain the swap chain back buffer.");
        renderTargetView_ = nullptr;
        return;
    }
    const HRESULT madeView = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView_);
    if (FAILED(madeView)) {
        infra::Logger::instance().error("Could not create the render target view.");
        renderTargetView_ = nullptr;
    }
    backBuffer->Release();
}

void UiApp::cleanupRenderTarget() {
    if (renderTargetView_) {
        renderTargetView_->Release();
        renderTargetView_ = nullptr;
    }
}

void UiApp::render() {
    // A lost render target (resize failure, device reset) must be rebuilt
    // before any drawing happens.
    if (!renderTargetView_) {
        createRenderTarget();
        if (!renderTargetView_) {
            return;
        }
    }

    // Before the frame is built, so that a script opening a panel affects the
    // frame about to be drawn rather than the one after it.
    drainAutomation();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Breakpoint hits arrive on the debugger's own thread, which must not touch
    // ImGui. They queue up there and surface here instead.
    for (auto& event : services_.breakpoints().takeEvents()) {
        notifyInfo(std::move(event));
    }
    // Same for script output, which now comes from the Lua worker thread and
    // appears as it is produced rather than all at once when the script ends.
    for (auto& line : lua_.takeOutput()) {
        luaOutput_.push_back(std::move(line));
    }

    // A handle to a process that has exited stays valid; without this the
    // status pills would keep saying ATTACHED / WATCHING while every read
    // failed one at a time. Once a second is more than fast enough -- a
    // process cannot become alive again between ticks.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastExitCheck_ > std::chrono::seconds(1)) {
        lastExitCheck_ = now;
        if (services_.session().attached() && services_.session().exited()) {
            notifyError("The target process has exited.");
            requestDetach();
        }
    }

    syncGlobalHotkeys();
    handleHotkeys();
    renderMenu();
    renderCommandBar();
    renderDockspace();
    // Core — always visible
    renderProcessPanel();
    renderScanPanel();
    renderAddressListPanel();

    // Where each of these docks is decided once, by buildDefaultDockLayout, and
    // afterwards by wherever the user dragged it. Forcing a dock id here on
    // every appearance overrode the default layout on the first frame and
    // collapsed six panels into two tab bars.
    if (showMemoryViewer_)   { renderMemoryPanel(); }
    if (showDisassembly_)    { renderDisassemblyPanel(); }
    if (showBreakpoints_)    { renderBreakpointPanel(); }
    if (showAccessWatch_)    { renderAccessWatchPanel(); }
    if (showPatches_)        { renderPatchesPanel(); }
    if (showSymbols_)        { renderSymbolsPanel(); }
    if (showScripts_)        { renderScriptsPanel(); }
    if (showStructures_)     { renderStructuresPanel(); }
    if (showSpeed_)          { renderSpeedPanel(); }
    if (showModules_)        { renderModulesPanel(); }
    if (showMemoryRegions_)  { renderRegionsPanel(); }
    if (showLogs_)           { renderLogPanel(); }
    if (showPointerScanner_) { renderPointerPanel(); }
    if (showLuaScanner_)     { renderLuaScannerPanel(); }
    if (showInjection_)      { renderInjectionPanel(); }
    if (showLuaConsole_)     { renderLuaPanel(); }
    if (showMcp_)            { renderMcpPanel(); }

    if (showAbout_) { renderAboutWindow(); }
    if (showHelp_)  { renderHelpWindow(); }

    // Drawn last so they sit above every panel.
    renderConfirmModal();
    renderToasts();

    // Focus has to be asked for between NewFrame and Render, so a select_panel
    // handled at the top of this frame lands here.
    if (!focusPanel_.empty()) {
        ImGui::SetWindowFocus(focusPanel_.c_str());
        focusPanel_.clear();
    }

    ImGui::Render();
    const float clearColor[4] = {0.06f, 0.07f, 0.08f, 1.0f};
    deviceContext_->OMSetRenderTargets(1, &renderTargetView_, nullptr);
    deviceContext_->ClearRenderTargetView(renderTargetView_, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    // Last thing before the present, because the swap chain discards on present
    // and after it there is nothing left in the back buffer to capture.
    drainScreenshots();

    const HRESULT presented = swapChain_->Present(1, 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
        // Driver crash, TDR, or GPU switch. Rebuild the device instead of
        // spinning forever on a dead swap chain.
        infra::Logger::instance().warn("Direct3D device lost; recreating.");
        ImGui_ImplDX11_Shutdown();
        cleanupDeviceD3D();
        if (createDeviceD3D(hwnd_)) {
            ImGui_ImplDX11_Init(device_, deviceContext_);
        } else {
            infra::Logger::instance().error("Could not recreate the Direct3D device.");
            PostQuitMessage(1);
        }
    } else if (presented == DXGI_STATUS_OCCLUDED) {
        // Fully hidden: stop burning a core rendering frames nobody sees.
        Sleep(16);
    }
}
void UiApp::requestDetach() {
    const auto liveBreakpoints = services_.breakpoints().breakpoints().size();
    const auto allPatches = services_.patches().patches();
    const auto appliedPatches = std::count_if(allPatches.begin(), allPatches.end(),
                                              [](const engine_patch::Patch& p) { return p.enabled; });

    const auto allScripts = services_.autoAssembler().scripts();
    const auto liveScripts = std::count_if(allScripts.begin(), allScripts.end(),
                                           [](const engine_aa::Script& s) { return s.enabled; });

    auto detach = [this, appliedPatches, liveScripts] {
        // Scripts are switched off first, unlike patches, and the asymmetry is
        // deliberate. A patch is one run of bytes with one undo record, so
        // leaving it applied is a coherent state. A script's changes are a set
        // of patches plus memory it allocated plus symbols it published, and
        // only the script knows how to take them apart. Once the handle closes
        // nothing can, so that is not "the cheat stays on" -- it is a leak with
        // no owner.
        if (liveScripts > 0) {
            if (auto disabled = services_.autoAssembler().disableAll(); !disabled) {
                notifyError(disabled.error());
            }
        }
        // The clocks go back for the same reason a script does: what the
        // payload changed is a set of import entries only it knows about, and
        // after this nothing is left that could put them back.
        if (auto reset = services_.speed().reset(); !reset) {
            notifyError(reset.error());
        }
        services_.speed().forget();
        services_.breakpoints().detachDebugger();
        // Deliberately not restored: a patch someone left applied may be the
        // whole point of the session. What is not optional is saying so, since
        // after this the originals are gone and the target keeps running.
        services_.patches().forgetAll();
        services_.session().detach();
        if (appliedPatches > 0) {
            notifyInfo("Detached. " + std::to_string(appliedPatches) +
                       " patch(es) are still applied in the target and can no longer be undone from here.");
        } else {
            notifyInfo("Detached from the target process.");
        }
    };

    std::string warning;
    if (liveBreakpoints != 0) {
        warning += std::to_string(liveBreakpoints) +
                   " breakpoint(s) are still set. Detaching removes them and restores the original "
                   "instruction bytes. If any byte cannot be restored the target will be left with a 0xCC "
                   "trap and will most likely crash.";
    }
    if (appliedPatches != 0) {
        if (!warning.empty()) {
            warning += "\n\n";
        }
        warning += std::to_string(appliedPatches) +
                   " patch(es) are still applied. Those are left in place -- the target keeps running with "
                   "them -- but the original bytes are forgotten, so this is the last chance to put them "
                   "back. Cancel and use Restore all in the Patches panel if you want the original code.";
    }
    if (liveScripts != 0) {
        if (!warning.empty()) {
            warning += "\n\n";
        }
        warning += std::to_string(liveScripts) +
                   " script(s) are on. Those are switched off and everything they changed is put back, "
                   "because after detaching nothing is left that knows how to undo them.";
    }

    if (warning.empty()) {
        detach();
        return;
    }
    confirmAction("Detach from the target?", warning, "Detach", detach);
}

void UiApp::notifyInfo(const std::string& text) {
    infra::Logger::instance().info(text);
    toasts_.push_back({text, false, 4.0f});
}

void UiApp::notifyError(const std::string& text) {
    infra::Logger::instance().error(text);
    // Errors linger noticeably longer than confirmations.
    toasts_.push_back({text, true, 9.0f});
}

void UiApp::renderToasts() {
    const float delta = ImGui::GetIO().DeltaTime;
    for (auto& toast : toasts_) {
        toast.secondsRemaining -= delta;
    }
    std::erase_if(toasts_, [](const Toast& toast) { return toast.secondsRemaining <= 0.0f; });
    if (toasts_.empty()) {
        return;
    }
    // Keep the stack short so a burst of failures cannot cover the app.
    while (toasts_.size() > 5) {
        toasts_.erase(toasts_.begin());
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float padding = 16.0f;
    float offsetY = padding;

    for (std::size_t i = 0; i < toasts_.size(); ++i) {
        const Toast& toast = toasts_[i];
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - padding, viewport->WorkPos.y + offsetY),
            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(220.0f), 0.0f), ImVec2(scaled(520.0f), FLT_MAX));

        const ImVec4 accent = toast.error ? colorFromBytes(196, 78, 78) : colorFromBytes(46, 138, 116);
        ImGui::PushStyleColor(ImGuiCol_Border, accent);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

        const std::string id = "##toast" + std::to_string(i);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin(id.c_str(), nullptr, flags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted(toast.error ? "Error" : "Done");
            ImGui::PopStyleColor();
            ImGui::PushTextWrapPos(480.0f);
            ImGui::TextUnformatted(toast.text.c_str());
            ImGui::PopTextWrapPos();
            offsetY += ImGui::GetWindowHeight() + 8.0f;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

void UiApp::confirmAction(std::string title, std::string message, std::string confirmLabel, std::function<void()> action) {
    // A second request queued while one is on screen used to silently replace
    // it, so the modal read for confirmation A ended up executing action B
    // when the user pressed the button. Refusing the second one keeps the
    // one already in front of the user honest.
    if (pendingConfirm_) {
        notifyError("Finish the current confirmation before starting another.");
        return;
    }
    pendingConfirm_ = PendingConfirm{std::move(title), std::move(message), std::move(confirmLabel), std::move(action), false};
}


void UiApp::renderConfirmModal() {
    if (!pendingConfirm_) {
        return;
    }
    if (!pendingConfirm_->opened) {
        ImGui::OpenPopup("##confirm");
        pendingConfirm_->opened = true;
    } else if (!ImGui::IsPopupOpen("##confirm")) {
        // ImGui closes popups on Escape. Without this the request would stay
        // pending forever and every later confirmation would be swallowed.
        pendingConfirm_.reset();
        return;
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(420.0f), 0.0f), ImVec2(scaled(640.0f), FLT_MAX));
    if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextUnformatted(pendingConfirm_->title.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::PushTextWrapPos(600.0f);
        ImGui::TextUnformatted(pendingConfirm_->message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, scaled(6.0f)));

        if (ImGui::Button(pendingConfirm_->confirmLabel.c_str(), ImVec2(scaled(180.0f), 0.0f))) {
            // Copy the action out before clearing: it may itself queue another
            // confirmation.
            auto action = pendingConfirm_->action;
            pendingConfirm_.reset();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            if (action) {
                action();
            }
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(scaled(120.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pendingConfirm_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
void UiApp::syncGlobalHotkeys() {
    // The revision check comes first, because the snapshot below deep-copies
    // three strings, a byte vector and an optional chain per entry -- and in
    // the overwhelmingly common case nothing has changed and every one of
    // those allocations is thrown away unread.
    const auto revision = services_.session().addressList().revision();
    if (revision == hotkeyListRevision_) {
        return;
    }
    hotkeyListRevision_ = revision;

    std::set<int> wanted;
    for (const auto& entry : services_.session().addressList().snapshot()) {
        if (auto id = platform_win32::GlobalHotkeys::idFor(entry.hotkey)) {
            wanted.insert(*id);
        }
    }
    if (wanted == hotkeysRequested_) {
        return;
    }
    hotkeysRequested_ = wanted;

    const auto refused = hotkeys_.apply(hwnd_, wanted);
    for (const int id : refused) {
        // Another application already owns it. The in-window handler still
        // serves this key, so say what was lost rather than letting the user
        // wonder why one of their hotkeys behaves differently.
        notifyError("F" + std::to_string(id) +
                    " is already registered by another application, so it only works while Pointer Lab has focus.");
    }
}

void UiApp::handleHotkeys() {
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    const ImGuiKey keys[] = {ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12};
    for (int i = 0; i < IM_ARRAYSIZE(keys); ++i) {
        // A globally registered key arrives as WM_HOTKEY whatever has focus,
        // including when that focus is Pointer Lab. Handling it here as well
        // would toggle the entry twice and leave it exactly as it was.
        if (hotkeys_.owns(i + 1)) {
            continue;
        }
        if (ImGui::IsKeyPressed(keys[i], false)) {
            services_.addressList().toggleHotkey("F" + std::to_string(i + 1));
        }
    }
}

void UiApp::refreshProcesses() {
    processes_ = services_.platform().listProcesses();

    // Narrowed and lowercased once here rather than per row per frame. A
    // machine with three hundred processes was making six hundred Win32
    // conversion calls every frame just to draw and filter the same list.
    processNames_.clear();
    processNamesLower_.clear();
    processNames_.reserve(processes_.size());
    processNamesLower_.reserve(processes_.size());
    for (const auto& process : processes_) {
        auto name = domain::narrow(process.name);
        auto lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        processNames_.push_back(std::move(name));
        processNamesLower_.push_back(std::move(lower));
    }
}


std::optional<std::uintptr_t> UiApp::parseAddress(const char* text) {
    if (!text || text[0] == '\0') {
        return std::nullopt;
    }
    return domain::parseAddress(text);
}

void UiApp::copyText(char* destination, std::size_t size, const std::string& text) {
    if (size == 0) {
        return;
    }
    const auto count = std::min(size - 1, text.size());
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

} // namespace ire::ui
