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
#include <iomanip>
#include <sstream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ire::ui {

namespace {

const char* scanModeNames[] = {"Exact", "Unknown initial", "Changed", "Unchanged", "Increased", "Decreased"};

constexpr ImGuiTableFlags denseTableFlags =
    ImGuiTableFlags_BordersInnerV |
    ImGuiTableFlags_BordersOuterH |
    ImGuiTableFlags_RowBg |
    ImGuiTableFlags_Resizable |
    ImGuiTableFlags_Reorderable |
    ImGuiTableFlags_Hideable |
    ImGuiTableFlags_SizingStretchProp;

domain::ScanMode scanModeFromIndex(int index) {
    switch (index) {
    case 1: return domain::ScanMode::UnknownInitial;
    case 2: return domain::ScanMode::Changed;
    case 3: return domain::ScanMode::Unchanged;
    case 4: return domain::ScanMode::Increased;
    case 5: return domain::ScanMode::Decreased;
    default: return domain::ScanMode::Exact;
    }
}

const char* valueTypeDisplayName(domain::ValueType type) {
    switch (type) {
    case domain::ValueType::Int8:   return "i8 (signed byte)";
    case domain::ValueType::UInt8:  return "u8 (byte)";
    case domain::ValueType::Int16:  return "i16 (short)";
    case domain::ValueType::UInt16: return "u16 (unsigned short)";
    case domain::ValueType::Int32:  return "i32 (int)";
    case domain::ValueType::UInt32: return "u32 (unsigned int)";
    case domain::ValueType::Int64:  return "i64 (long long)";
    case domain::ValueType::UInt64: return "u64 (unsigned long long)";
    case domain::ValueType::Float:  return "f32 (float)";
    case domain::ValueType::Double: return "f64 (double)";
    case domain::ValueType::Bytes:  return "bytes (byte array)";
    }
    return "unknown";
}

std::vector<const char*> valueTypeNames() {
    std::vector<const char*> names;
    for (const auto type : domain::valueTypes()) {
        names.push_back(valueTypeDisplayName(type));
    }
    return names;
}

domain::ValueType valueTypeFromIndex(int index) {
    const auto types = domain::valueTypes();
    if (index < 0 || static_cast<std::size_t>(index) >= types.size()) {
        return domain::ValueType::Int32;
    }
    return types[static_cast<std::size_t>(index)];
}

std::string formatSize(std::size_t size) {
    std::ostringstream out;
    if (size > 1024 * 1024) {
        out << (size / (1024 * 1024)) << " MB";
    } else if (size > 1024) {
        out << (size / 1024) << " KB";
    } else {
        out << size << " B";
    }
    return out.str();
}

ImVec4 colorFromBytes(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void statusPill(const char* label, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
    ImGui::SmallButton(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void helpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool textMatchesFilter(const std::wstring& text, const char* filter) {
    if (!filter || filter[0] == '\0') {
        return true;
    }
    auto haystack = domain::narrow(text);
    auto needle = std::string(filter);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

} // namespace

UiApp::UiApp(HINSTANCE instance, int showCommand)
    : instance_(instance), showCommand_(showCommand), lua_(services_) {
    copyText(addDescription_.data(), addDescription_.size(), "Manual entry");
    copyText(addGroup_.data(), addGroup_.size(), "Default");
    copyText(luaScanScript_.data(), luaScanScript_.size(),
        "return function(ctx)\n"
        "    -- ctx.address, ctx.value, ctx.bytes, ctx.hex, ctx.type\n"
        "    return ctx.value ~= nil and ctx.value > 1000\n"
        "end\n");
}

UiApp::~UiApp() {
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
    if (showModules_)        { renderModulesPanel(); }
    if (showMemoryRegions_)  { renderRegionsPanel(); }
    if (showLogs_)           { renderLogPanel(); }
    if (showPointerScanner_) { renderPointerPanel(); }
    if (showLuaScanner_)     { renderLuaScannerPanel(); }
    if (showInjection_)      { renderInjectionPanel(); }
    if (showLuaConsole_)     { renderLuaPanel(); }

    if (showAbout_) { renderAboutWindow(); }
    if (showHelp_)  { renderHelpWindow(); }

    // Drawn last so they sit above every panel.
    renderConfirmModal();
    renderToasts();

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

void UiApp::applyStyle() {
    ImGuiIO& io = ImGui::GetIO();

    // Fonts are compiled into the binary (see EmbeddedFonts.h). Loading them
    // from disk meant that moving the executable away from resources/fonts/
    // left monoFont_ null, and the first panel to push it dereferenced null.
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(RobotoMedium_compressed_data_base85, 15.0f);
    monoFont_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(CousineRegular_compressed_data_base85, 14.0f);
    if (!monoFont_) {
        // Embedded data can only fail to build under memory exhaustion; fall
        // back to the always-present default font rather than a null pointer.
        monoFont_ = io.Fonts->AddFontDefault();
    }

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

    ImGui::DockBuilderDockWindow("Memory Viewer",     right);
    ImGui::DockBuilderDockWindow("Disassembly",       right);
    ImGui::DockBuilderDockWindow("Breakpoints",       right);

    ImGui::DockBuilderDockWindow("Pointer Scanner",   bottomLeft);
    ImGui::DockBuilderDockWindow("Lua Scanner",       bottomLeft);

    ImGui::DockBuilderDockWindow("Injection",         bottomRight);
    ImGui::DockBuilderDockWindow("Lua Console",       bottomRight);
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
        if (ImGui::MenuItem("Memory Viewer"))  showMemoryViewer_  = true;
        if (ImGui::MenuItem("Disassembly"))   showDisassembly_   = true;
        if (ImGui::MenuItem("Breakpoints"))   showBreakpoints_   = true;
        if (ImGui::MenuItem("Modules"))       showModules_       = true;
        if (ImGui::MenuItem("Memory Regions")) showMemoryRegions_ = true;
        if (ImGui::MenuItem("Logs"))          showLogs_          = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Pointer Scanner")) showPointerScanner_ = true;
        if (ImGui::MenuItem("Lua Scanner"))    showLuaScanner_     = true;
        if (ImGui::MenuItem("Injection"))      showInjection_      = true;
        if (ImGui::MenuItem("Lua Console"))    showLuaConsole_     = true;
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
    ImGui::Dummy(ImVec2(20.0f, 0.0f));
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
    if (ImGui::SmallButton("Detach") && attached) {
        requestDetach();
    }

    ImGui::End();
}

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

void UiApp::renderScanPanel() {
    ImGui::Begin("Scanner");
    const auto typeNames = valueTypeNames();

    ImGui::BeginChild("scan-controls", ImVec2(0, 132.0f), true);
    ImGui::TextDisabled("Scan setup");
    ImGui::SameLine();
    helpMarker("Use Unknown initial for broad baselines, then Changed/Unchanged/Increased/Decreased for narrowing. Exact scans compare against the typed value.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(118.0f);
    ImGui::Combo("Type", &scanTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::Combo("Mode", &scanModeIndex_, scanModeNames, IM_ARRAYSIZE(scanModeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##scan-value", "value or byte pattern", scanText_.data(), scanText_.size());

    if (showScannerFilters_ || ImGui::TreeNodeEx("Region filters", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Writable only", &scanWritableOnly_);
        ImGui::SameLine();
        ImGui::Checkbox("Executable only", &scanExecutableOnly_);
        if (!showScannerFilters_) {
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    const auto type = valueTypeFromIndex(scanTypeIndex_);
    const auto mode = scanModeFromIndex(scanModeIndex_);

    // Changed/Unchanged/Increased/Decreased compare against the previous scan
    // and need no typed value. Requiring one meant the flagship
    // unknown-then-narrow workflow silently did nothing.
    const bool valueless = engine_scan::modeNeedsBaseline(mode) || mode == domain::ScanMode::UnknownInitial;
    auto scanValue = domain::parseScanValue(type, scanText_.data());
    if (!scanValue && valueless && type != domain::ValueType::Bytes) {
        scanValue = domain::parseScanValue(type, "0");
    }

    const auto currentOptions = [this] {
        engine_scan::ScanOptions options;
        options.writableOnly = scanWritableOnly_;
        options.executableOnly = scanExecutableOnly_;
        options.maxResults = static_cast<std::size_t>(std::max(1000, scanMaxResults_));
        options.floatEpsilon = static_cast<double>(scanFloatEpsilon_);
        return options;
    };

    if (ImGui::Button("First scan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process before scanning.");
        } else if (scanValue) {
            services_.scanJob().setOptions(currentOptions());
            services_.scanJob().startFirst(mode, *scanValue);
            if (engine_scan::modeNeedsBaseline(mode)) {
                notifyInfo(std::string(domain::scanModeName(mode)) +
                           " needs something to compare against, so this first scan records a baseline. "
                           "Let the value change, then press Next scan.");
            }
        } else {
            notifyError(type == domain::ValueType::Bytes
                            ? "Enter a byte pattern such as 48 8B ?? 24."
                            : "Scan value is not valid for the selected type.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next scan")) {
        auto previous = services_.scanJob().results();
        if (previous.empty()) {
            notifyError("Run a first scan before filtering with Next scan.");
        } else if (scanValue) {
            services_.scanJob().setOptions(currentOptions());
            services_.scanJob().startNext(mode, *scanValue, std::move(previous));
        } else {
            notifyError("Scan value is not valid for the selected type.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services_.scanJob().cancel();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputInt("Result limit", &scanMaxResults_, 0, 0);
    scanMaxResults_ = std::clamp(scanMaxResults_, 1000, 20000000);
    ImGui::SameLine();
    helpMarker("Scanning stops once this many results are found. Unknown-initial scans of a large "
               "process can exceed it easily; narrow with region filters or raise the limit.");
    if (type == domain::ValueType::Float || type == domain::ValueType::Double) {
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputFloat("Float tolerance", &scanFloatEpsilon_, 0.0f, 0.0f, "%.5f");
        scanFloatEpsilon_ = std::clamp(scanFloatEpsilon_, 0.0f, 1000.0f);
        ImGui::SameLine();
        helpMarker("Exact float matches allow this much difference. A displayed 100.0 is rarely "
                   "bit-identical to the stored value, so a tolerance of 0 usually finds nothing.");
    }

    const auto progress = services_.scanJob().progress();
    ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
    if (progress.truncated) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextWrapped("Result limit reached, so this is only part of the address space. "
                           "Narrow the scan with region filters or raise the limit.");
        ImGui::PopStyleColor();
    }

    auto results = services_.scanJob().results();
    constexpr std::size_t displayLimit = 10000;
    const std::size_t count = std::min<std::size_t>(results.size(), displayLimit);
    if (results.size() > displayLimit) {
        // Silently showing the first 10,000 of a much larger set read as
        // "these are all the results".
        ImGui::TextDisabled("Showing the first %zu of %zu results. Narrow the scan to see the rest.",
                            count, results.size());
    } else {
        ImGui::TextDisabled("%zu result%s", results.size(), results.size() == 1 ? "" : "s");
    }

    if (ImGui::BeginTable("scan-results", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Previous");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Only the visible rows are laid out; formatting 10,000 rows every
        // frame was a large amount of pointless work.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto i = static_cast<std::size_t>(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::toHex(results[i].address).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::formatValue(type, results[i].previous).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::formatValue(type, results[i].current).c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                if (ImGui::SmallButton("Add")) {
                    services_.addressList().add(results[i].address, type, "Scan result", "Scan");
                    notifyInfo("Added " + domain::toHex(results[i].address) + " to the address list.");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("View")) {
                    copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(results[i].address));
                    copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(results[i].address));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderAddressListPanel() {
    ImGui::Begin("Address List");
    const auto typeNames = valueTypeNames();
    auto entries = services_.session().addressList().snapshot();
    statusPill(entries.empty() ? "EMPTY" : "TRACKING", entries.empty() ? colorFromBytes(63, 75, 88) : colorFromBytes(30, 111, 96));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu address%s", entries.size(), entries.size() == 1 ? "" : "es");
    ImGui::SameLine();
    if (ImGui::SmallButton(showManualAddressEditor_ ? "Hide editor" : "Manual add")) {
        showManualAddressEditor_ = !showManualAddressEditor_;
    }
    ImGui::Separator();

    ImGuiTreeNodeFlags editorFlags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (showManualAddressEditor_ || editEntryId_ != 0) {
        editorFlags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (ImGui::CollapsingHeader(editEntryId_ == 0 ? "Manual add / edit" : "Editing selected entry", editorFlags)) {
        ImGui::BeginChild("address-editor", ImVec2(0, 126.0f), true);
        ImGui::SetNextItemWidth(210.0f);
        ImGui::InputTextWithHint("Address", "0x7FF...", addAddress_.data(), addAddress_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Type", &addTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputText("Description", addDescription_.data(), addDescription_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("Group", addGroup_.data(), addGroup_.size());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputTextWithHint("Value", "optional freeze/write value", addValue_.data(), addValue_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputTextWithHint("Hotkey", "F1-F12", addHotkey_.data(), addHotkey_.size());
        ImGui::SameLine();
        if (ImGui::Button(editEntryId_ == 0 ? "Add" : "Apply")) {
            if (auto address = parseAddress(addAddress_.data())) {
                const auto type = valueTypeFromIndex(addTypeIndex_);
                if (editEntryId_ == 0) {
                    const auto id = services_.addressList().add(*address, type, addDescription_.data(), addGroup_.data());
                    auto updated = services_.session().addressList().snapshot();
                    for (auto& entry : updated) {
                        if (entry.id == id) {
                            entry.hotkey = addHotkey_.data();
                            if (auto value = domain::parseScanValue(type, addValue_.data())) {
                                entry.frozenValue = std::move(value->bytes);
                            }
                            services_.session().addressList().update(entry);
                        }
                    }
                } else {
                    auto updated = services_.session().addressList().snapshot();
                    for (auto& entry : updated) {
                        if (entry.id == editEntryId_) {
                            entry.address = *address;
                            entry.type = type;
                            entry.description = addDescription_.data();
                            entry.group = addGroup_.data();
                            entry.hotkey = addHotkey_.data();
                            if (auto value = domain::parseScanValue(type, addValue_.data())) {
                                entry.frozenValue = std::move(value->bytes);
                            }
                            services_.session().addressList().update(entry);
                            editEntryId_ = 0;
                            break;
                        }
                    }
                }
            }
        }
        if (editEntryId_ != 0) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                editEntryId_ = 0;
            }
        }
        ImGui::EndChild();
    }

    if (ImGui::BeginTable("addresses", 8, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Description");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Freeze");
        ImGui::TableSetupColumn("Hotkey");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (auto& entry : entries) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.group.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.description.c_str());
            ImGui::TableNextColumn();
            if (entry.chain && !entry.resolved) {
                // Showing the last known address as though it were live would be
                // a lie: the chain no longer leads anywhere.
                ImGui::TextDisabled("unresolved");
            } else {
                ImGui::TextUnformatted(domain::toHex(entry.address).c_str());
            }
            if (entry.chain) {
                ImGui::SameLine();
                ImGui::TextDisabled("(P)");
                if (ImGui::IsItemHovered()) {
                    std::ostringstream tip;
                    tip << domain::narrow(entry.chain->moduleName) << '+'
                        << domain::toHex(entry.chain->moduleOffset);
                    for (const auto offset : entry.chain->offsets) {
                        tip << " -> +0x" << std::hex << offset;
                    }
                    tip << "\n\nTracked as a pointer chain, so it re-resolves when the target restarts.";
                    ImGui::SetTooltip("%s", tip.str().c_str());
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::valueTypeName(entry.type));
            ImGui::TableNextColumn();
            std::string current = "<unreadable>";
            if (entry.chain && !entry.resolved) {
                current = "<chain broken>";
            } else if (services_.session().attached()) {
                if (auto bytes = services_.session().readBytes(entry.address, std::max<std::size_t>(1, domain::valueTypeSize(entry.type)))) {
                    current = domain::formatValue(entry.type, bytes.value());
                }
            }
            ImGui::TextUnformatted(current.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(entry.id)));
            bool frozen = entry.frozen;
            if (ImGui::Checkbox("##freeze", &frozen)) {
                services_.addressList().setFrozen(entry.id, frozen);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.hotkey.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Edit")) {
                editEntryId_ = entry.id;
                showManualAddressEditor_ = true;
                copyText(addAddress_.data(), addAddress_.size(), domain::toHex(entry.address));
                copyText(addDescription_.data(), addDescription_.size(), entry.description);
                copyText(addGroup_.data(), addGroup_.size(), entry.group);
                copyText(addHotkey_.data(), addHotkey_.size(), entry.hotkey);
                copyText(addValue_.data(), addValue_.size(), domain::formatValue(entry.type, entry.frozenValue));
                const auto types = domain::valueTypes();
                const auto it = std::find(types.begin(), types.end(), entry.type);
                addTypeIndex_ = it == types.end() ? 4 : static_cast<int>(std::distance(types.begin(), it));
            }
            ImGui::SameLine();
            // This used to write whatever happened to be typed in the shared
            // manual-editor box at the top of the panel, so pressing Write on
            // one row could store a value meant for a different one.
            if (ImGui::SmallButton("Write")) {
                rowWriteId_ = entry.id;
                copyText(rowWriteValue_.data(), rowWriteValue_.size(), current);
                ImGui::OpenPopup("##write-value");
            }
            if (ImGui::BeginPopup("##write-value")) {
                ImGui::TextDisabled("%s at %s", domain::valueTypeName(entry.type), domain::toHex(entry.address).c_str());
                ImGui::SetNextItemWidth(180.0f);
                const bool submitted = ImGui::InputText("##value", rowWriteValue_.data(), rowWriteValue_.size(),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Write") || submitted) && rowWriteId_ == entry.id) {
                    if (auto value = domain::parseScanValue(entry.type, rowWriteValue_.data())) {
                        if (services_.addressList().updateValue(entry.id, value->bytes)) {
                            notifyInfo("Wrote " + std::string(rowWriteValue_.data()) + " to " + domain::toHex(entry.address) + ".");
                        } else {
                            notifyError("Could not write to " + domain::toHex(entry.address) + ".");
                        }
                    } else {
                        notifyError("Value is not valid for type " + std::string(domain::valueTypeName(entry.type)) + ".");
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                const auto id = entry.id;
                const auto label = entry.description.empty() ? domain::toHex(entry.address) : entry.description;
                confirmAction("Remove address entry?",
                              "Remove \"" + label + "\" from the address list? Any freeze on it stops.",
                              "Remove",
                              [this, id] {
                                  if (services_.addressList().remove(id)) {
                                      notifyInfo("Entry removed.");
                                  }
                              });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderMemoryPanel() {
    ImGui::Begin("Memory Viewer", &showMemoryViewer_);
    ImGui::SetNextItemWidth(210.0f);
    ImGui::InputTextWithHint("Address", "0x7FF...", memoryAddress_.data(), memoryAddress_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Bytes", &memoryReadSize_);
    memoryReadSize_ = std::clamp(memoryReadSize_, 16, 4096);
    std::vector<std::uint8_t> bytes;
    if (auto address = parseAddress(memoryAddress_.data()); address && services_.session().attached()) {
        if (auto read = services_.session().readBytes(*address, static_cast<std::size_t>(memoryReadSize_))) {
            bytes = std::move(read.value());
        }
    }

    if (ImGui::BeginTabBar("memory-tabs")) {
        if (ImGui::BeginTabItem("Hex")) {
            if (!bytes.empty()) {
                ImGui::BeginChild("hex-view", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                const auto base = parseAddress(memoryAddress_.data()).value_or(0);
                for (std::size_t row = 0; row < bytes.size(); row += 16) {
                    ImGui::Text("%s  ", domain::toHex(base + row).c_str());
                    ImGui::SameLine();
                    for (std::size_t col = 0; col < 16 && row + col < bytes.size(); ++col) {
                        ImGui::Text("%02X ", bytes[row + col]);
                        if (col != 15) {
                            ImGui::SameLine();
                        }
                    }
                    ImGui::SameLine(0, 24);
                    std::string ascii;
                    for (std::size_t col = 0; col < 16 && row + col < bytes.size(); ++col) {
                        const unsigned char c = bytes[row + col];
                        ascii.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
                    }
                    ImGui::TextUnformatted(ascii.c_str());
                }
                ImGui::PopFont();
                ImGui::EndChild();
            } else {
                ImGui::TextDisabled("Attach to a target and enter an address to read memory.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Typed")) {
            if (ImGui::BeginTable("typed-view", 3, denseTableFlags)) {
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("Value at address");
                ImGui::TableHeadersRow();
                for (const auto type : domain::valueTypes()) {
                    const auto size = domain::valueTypeSize(type);
                    if (type == domain::ValueType::Bytes || size == 0 || bytes.size() < size) {
                        continue;
                    }
                    std::vector<std::uint8_t> value(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::valueTypeName(type));
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", size);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::formatValue(type, value).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Patch")) {
            ImGui::TextDisabled("Patch raw bytes at the current address.");
            ImGui::InputTextWithHint("Bytes", "90 90 CC", memoryPatch_.data(), memoryPatch_.size());
            if (ImGui::Button("Patch at address")) {
                if (auto address = parseAddress(memoryAddress_.data())) {
                    auto patch = domain::parseHexBytes(memoryPatch_.data());
                    if (patch.empty()) {
                        notifyError("Enter the replacement bytes as hexadecimal, for example 90 90 CC.");
                    } else {
                        const auto target = *address;
                        confirmAction(
                            "Overwrite memory in the target?",
                            "This writes " + std::to_string(patch.size()) + " byte(s) over " +
                            domain::toHex(target) + ". The previous contents are not saved and "
                            "Pointer Lab cannot undo the change.",
                            "Patch",
                            [this, target, patch] {
                                if (auto result = services_.session().writeBytes(target, patch); !result) {
                                    notifyError("Patch failed: " + result.error());
                                } else {
                                    notifyInfo("Wrote " + std::to_string(patch.size()) + " bytes at " + domain::toHex(target) + ".");
                                }
                            });
                    }
                } else {
                    notifyError("Address is not valid hexadecimal.");
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiApp::renderDisassemblyPanel() {
    ImGui::Begin("Disassembly", &showDisassembly_);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Address", "0x7FF...", disasmAddress_.data(), disasmAddress_.size());
    const auto address = parseAddress(disasmAddress_.data());
    if (ImGui::BeginTabBar("disasm-tabs")) {
        if (ImGui::BeginTabItem("Listing")) {
            if (address && services_.session().attached()) {
                const auto instructions = services_.disassembler().disassemble(services_.session(), *address, 64);
                if (instructions.empty()) {
                    ImGui::TextDisabled("Nothing readable at %s.", domain::toHex(*address).c_str());
                }
                ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                if (ImGui::BeginTable("disasm", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 190.0f);
                    ImGui::TableSetupColumn("Instruction");
                    ImGui::TableSetupColumn("##follow", ImGuiTableColumnFlags_WidthFixed, 66.0f);
                    ImGui::TableHeadersRow();
                    for (const auto& ins : instructions) {
                        ImGui::PushID(reinterpret_cast<const void*>(ins.address));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(domain::toHex(ins.address).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(domain::bytesToHex(ins.bytes).c_str());
                        ImGui::TableNextColumn();
                        if (ins.valid) {
                            ImGui::TextUnformatted(ins.text.c_str());
                        } else {
                            // Bytes that did not decode are shown as data rather
                            // than guessed at, which is what used to slide the
                            // rest of the listing out of alignment.
                            ImGui::TextDisabled("%s", ins.text.c_str());
                        }
                        ImGui::TableNextColumn();
                        if (ins.branchTarget != 0 && ImGui::SmallButton("Follow")) {
                            copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(ins.branchTarget));
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopFont();
            } else {
                ImGui::TextDisabled("Attach and enter an address to disassemble from it.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Assembler Patch")) {
            ImGui::TextDisabled("Intel syntax, one instruction per line. ';' and '//' begin a comment.");
            ImGui::TextDisabled("Assembled at the Address above, so relative jumps and calls resolve correctly.");
            ImGui::InputTextMultiline("##Assembler", assemblerText_.data(), assemblerText_.size(), ImVec2(-1, -34.0f));
            if (ImGui::Button("Assemble and patch")) {
                if (!address) {
                    notifyError("Enter the address to assemble at.");
                } else if (!services_.session().attached()) {
                    notifyError("Attach to a process before patching it.");
                } else {
                    auto assembled = services_.assembler().assemble(assemblerText_.data(), *address);
                    if (!assembled) {
                        notifyError(assembled.error());
                    } else {
                        const auto target = *address;
                        const auto code = assembled.value();
                        const auto bytes = engine_disasm::padToInstructionBoundary(
                            services_.disassembler(), services_.session(), target, code);
                        const auto padding = bytes.size() - code.size();

                        std::string message = "This assembles to " + std::to_string(code.size()) + " byte(s):\n\n" +
                                              domain::bytesToHex(code) + "\n\n";
                        if (padding > 0) {
                            message += "It is shorter than the instructions it replaces, so " +
                                       std::to_string(padding) +
                                       " nop byte(s) will be appended to fill out the last one. Without that the "
                                       "target would resume mid-instruction and crash.\n\n";
                        }
                        message += "Overwriting code at " + domain::toHex(target) + " cannot be undone.";

                        confirmAction("Overwrite code in the target?", std::move(message), "Patch",
                            [this, target, bytes] {
                                if (auto result = services_.session().writeBytes(target, bytes); !result) {
                                    notifyError("Patch failed: " + result.error());
                                } else {
                                    notifyInfo("Patched " + std::to_string(bytes.size()) + " bytes at " + domain::toHex(target) + ".");
                                }
                            });
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiApp::renderBreakpointPanel() {
    ImGui::Begin("Breakpoints", &showBreakpoints_);
    statusPill(services_.breakpoints().debuggerAttached() ? "DEBUGGER ATTACHED" : "DEBUGGER OFF",
        services_.breakpoints().debuggerAttached() ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    if (ImGui::Button(services_.breakpoints().debuggerAttached() ? "Detach debugger" : "Attach debugger")) {
        if (services_.breakpoints().debuggerAttached()) {
            services_.breakpoints().detachDebugger();
        } else if (auto result = services_.breakpoints().attachDebugger(); !result) {
            notifyError("Debugger attach failed: " + result.error());
        }
    }
    ImGui::SetNextItemWidth(210.0f);
    ImGui::InputTextWithHint("Address", "0x7FF...", breakpointAddress_.data(), breakpointAddress_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-60.0f);
    ImGui::InputTextWithHint("Label", "optional name", breakpointLabel_.data(), breakpointLabel_.size());

    static constexpr std::array<domain::BreakpointKind, 4> breakpointKinds{
        domain::BreakpointKind::Software, domain::BreakpointKind::HardwareExecute,
        domain::BreakpointKind::HardwareWrite, domain::BreakpointKind::HardwareReadWrite};
    static constexpr std::array<const char*, 4> breakpointKindLabels{
        "Software (int3)", "Hardware execute", "Hardware write", "Hardware read/write"};
    static constexpr std::array<std::uint8_t, 4> breakpointWidths{1, 2, 4, 8};

    ImGui::SetNextItemWidth(210.0f);
    ImGui::Combo("Kind", &breakpointKindIndex_, breakpointKindLabels.data(),
                 static_cast<int>(breakpointKindLabels.size()));
    const auto kind = breakpointKinds[static_cast<std::size_t>(breakpointKindIndex_)];
    const bool watchesData =
        kind == domain::BreakpointKind::HardwareWrite || kind == domain::BreakpointKind::HardwareReadWrite;
    if (watchesData) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Width", &breakpointWidthIndex_, "1 byte\0" "2 bytes\0" "4 bytes\0" "8 bytes\0");
    }
    ImGui::SameLine();
    if (ImGui::Button("Set breakpoint")) {
        if (auto address = parseAddress(breakpointAddress_.data())) {
            const auto width = breakpointWidths[static_cast<std::size_t>(breakpointWidthIndex_)];
            if (auto result = services_.breakpoints().addBreakpoint(*address, breakpointLabel_.data(), kind, width);
                !result) {
                notifyError("Breakpoint failed: " + result.error());
            } else {
                notifyInfo(std::string(domain::breakpointKindName(kind)) + " breakpoint set at " +
                           domain::toHex(*address) + ".");
            }
        } else {
            notifyError("Address is not valid hexadecimal.");
        }
    }
    ImGui::SameLine();
    helpMarker("A software breakpoint replaces an instruction byte with int3. There can be any number of "
               "them, but the byte has to be restored and re-armed around every hit, and another thread "
               "running through the address during that window misses it.\n\n"
               "A hardware breakpoint uses one of the processor's four debug registers instead. Nothing in "
               "the target is modified and nothing is ever disarmed, so that window does not exist -- but "
               "there are exactly four, and the fifth is refused. They are also the only way to break on "
               "data being read or written rather than on code running.\n\n"
               "A watched address must be aligned to its width.");
    ImGui::TextDisabled(
        "The target keeps running: a software hit is stepped over and re-armed behind it, and a hardware "
        "hit never disarms anything in the first place.");

    const auto breakpoints = services_.breakpoints().breakpoints();
    if (ImGui::BeginTable("breakpoints", 7, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Armed", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Last thread", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (const auto& bp : breakpoints) {
            ImGui::PushID(reinterpret_cast<const void*>(bp.address));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(bp.address).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bp.label.c_str());
            ImGui::TableNextColumn();
            if (domain::isHardware(bp.kind)) {
                // Naming the register makes the four-at-a-time limit visible
                // rather than something the user only meets as a refusal.
                if (bp.kind == domain::BreakpointKind::HardwareExecute) {
                    ImGui::Text("%s (DR%d)", domain::breakpointKindName(bp.kind), bp.slot);
                } else {
                    ImGui::Text("%s %u (DR%d)", domain::breakpointKindName(bp.kind),
                                static_cast<unsigned>(bp.length), bp.slot);
                }
            } else {
                ImGui::TextDisabled("%s", domain::breakpointKindName(bp.kind));
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bp.enabled ? "yes" : "no");
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(bp.hitCount));
            ImGui::TableNextColumn();
            if (bp.lastHit.captured) {
                ImGui::Text("%u", bp.lastHit.threadId);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                // Confirmed for the same reason removing an address entry is:
                // it writes to the target's instruction stream, and the row it
                // is attached to disappears the moment it succeeds.
                const auto address = bp.address;
                const auto label = bp.label.empty() ? domain::toHex(address) : bp.label;
                // A hardware breakpoint never wrote anything into the target, so
                // promising to write a byte back would be a lie.
                const std::string consequence =
                    domain::isHardware(bp.kind)
                        ? "? Its debug register is released and becomes available again."
                        : "? The original instruction byte is written back to the target.";
                confirmAction("Remove breakpoint?", "Remove the breakpoint on " + label + consequence, "Remove",
                              [this, address] {
                                  if (auto removed = services_.breakpoints().removeBreakpoint(address); !removed) {
                                      notifyError(removed.error());
                                  } else {
                                      notifyInfo("Breakpoint removed at " + domain::toHex(address) + ".");
                                  }
                              });
            }
            if (bp.lastHit.captured) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Registers")) {
                    ImGui::OpenPopup("##registers");
                }
                if (ImGui::BeginPopup("##registers")) {
                    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                    const auto& r = bp.lastHit;
                    ImGui::Text("thread %u, %llu hit(s)", r.threadId, static_cast<unsigned long long>(bp.hitCount));
                    ImGui::Separator();
                    const std::pair<const char*, std::uint64_t> registers[] = {
                        {"rip", r.rip}, {"rsp", r.rsp}, {"rbp", r.rbp}, {"rax", r.rax},
                        {"rbx", r.rbx}, {"rcx", r.rcx}, {"rdx", r.rdx}, {"rsi", r.rsi},
                        {"rdi", r.rdi}, {"r8 ", r.r8},  {"r9 ", r.r9},  {"r10", r.r10},
                        {"r11", r.r11}, {"r12", r.r12}, {"r13", r.r13}, {"r14", r.r14},
                        {"r15", r.r15},
                    };
                    for (std::size_t i = 0; i < IM_ARRAYSIZE(registers); ++i) {
                        ImGui::Text("%s  %016llX", registers[i].first,
                                    static_cast<unsigned long long>(registers[i].second));
                        if (i % 2 == 0 && i + 1 < IM_ARRAYSIZE(registers)) {
                            ImGui::SameLine(0.0f, 24.0f);
                        }
                    }
                    ImGui::Text("eflags %08X", r.eflags);
                    ImGui::PopFont();
                    ImGui::EndPopup();
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

void UiApp::renderPointerPanel() {
    ImGui::Begin("Pointer Scanner", &showPointerScanner_);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Target address", "0x7FF...", pointerTarget_.data(), pointerTarget_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("Max depth", &pointerDepth_);
    pointerDepth_ = std::clamp(pointerDepth_, 1, 8);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Max offset", pointerMaxOffset_.data(), pointerMaxOffset_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(118.0f);
    const auto typeNames = valueTypeNames();
    ImGui::Combo("Value type", &pointerTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));

    // Fetched once: the table below needs it too, and copying every chain twice
    // a frame is not free once a scan has found thousands of them.
    const auto chains = services_.pointerScanJob().results();

    if (ImGui::Button("Start pointer scan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process first.");
        } else if (auto target = parseAddress(pointerTarget_.data())) {
            engine_pointer::PointerScanOptions options;
            options.target = *target;
            options.maxDepth = static_cast<std::uint32_t>(pointerDepth_);
            options.maxOffset = static_cast<std::uint32_t>(parseAddress(pointerMaxOffset_.data()).value_or(0x1000));
            services_.pointerScanJob().start(options);
        } else {
            notifyError("Target address is not valid hexadecimal.");
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(chains.empty() || services_.pointerScanJob().progress().running);
    if (ImGui::Button("Rescan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process first.");
        } else if (auto target = parseAddress(pointerTarget_.data())) {
            services_.pointerScanJob().filter(*target);
        } else {
            notifyError("Target address is not valid hexadecimal.");
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    helpMarker("Keeps only the chains that still resolve to the target address above, and discards the "
               "rest. A first scan finds thousands of chains that pointed the right way once. Restart "
               "the target, find the value's new address, put it in Target address and rescan: what "
               "survives is what actually tracks the value. Rescanning costs seconds, not minutes.");
    if (services_.pointerScanJob().progress().running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel pointer scan")) {
            services_.pointerScanJob().cancel();
        }
    }
    const auto progress = services_.pointerScanJob().progress();
    ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
    ImGui::TextDisabled(
        "Adding a chain tracks it as module+offset, so it re-resolves itself when the target restarts.");

    if (ImGui::BeginTable("pointer-results", 5, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Offsets", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Resolves to", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(chains.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& chain = chains[static_cast<std::size_t>(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::narrow(chain.moduleName).c_str());
                ImGui::TableNextColumn();
                // Module-relative, because that is what actually survives a
                // restart. The absolute address is only true for this run.
                ImGui::Text("%s+%s", domain::narrow(chain.moduleName).c_str(),
                            domain::toHex(chain.moduleOffset).c_str());
                ImGui::TableNextColumn();
                std::ostringstream offsets;
                for (std::size_t i = 0; i < chain.offsets.size(); ++i) {
                    if (i != 0) {
                        offsets << ", ";
                    }
                    offsets << "0x" << std::hex << chain.offsets[i];
                }
                ImGui::TextUnformatted(offsets.str().c_str());
                ImGui::TableNextColumn();
                if (auto resolved = engine_pointer::resolveChain(services_.session(), chain)) {
                    ImGui::TextUnformatted(domain::toHex(resolved.value()).c_str());
                } else {
                    ImGui::TextDisabled("unresolved");
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Add")) {
                    const auto type = domain::valueTypes()[static_cast<std::size_t>(pointerTypeIndex_)];
                    services_.addressList().addChain(chain, type,
                                                     domain::narrow(chain.moduleName) + "+" +
                                                         domain::toHex(chain.moduleOffset),
                                                     "Pointers");
                    notifyInfo("Added the pointer chain to the address list.");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderInjectionPanel() {
    ImGui::Begin("Injection", &showInjection_);
    ImGui::TextDisabled("These actions run code inside the target process.");
    helpMarker("Injection can destabilise or crash the target, and anti-cheat software commonly "
               "treats it as an attack. Only use it on software you own or are authorised to modify.");
    ImGui::Separator();

    if (ImGui::BeginTabBar("inject-tabs")) {
        if (ImGui::BeginTabItem("Allocate")) {
            ImGui::InputText("Allocation size", allocSize_.data(), allocSize_.size());
            if (ImGui::Button("Remote allocate RWX")) {
                const auto size = static_cast<std::size_t>(parseAddress(allocSize_.data()).value_or(4096));
                confirmAction(
                    "Allocate executable memory in the target?",
                    "This reserves " + std::to_string(size) + " bytes of read/write/execute memory inside " +
                    domain::narrow(services_.session().processName()) +
                    ". Executable memory in another process is exactly what malware allocates, so security "
                    "software may react. Pointer Lab cannot free it automatically.",
                    "Allocate",
                    [this, size] {
                        auto result = services_.injector().allocate(size, PAGE_EXECUTE_READWRITE);
                        if (result) {
                            notifyInfo("Remote allocation at " + domain::toHex(result.value()) + ".");
                            copyText(threadStart_.data(), threadStart_.size(), domain::toHex(result.value()));
                        } else {
                            notifyError("Allocation failed: " + result.error());
                        }
                    });
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Remote Thread")) {
            ImGui::InputText("Thread start", threadStart_.data(), threadStart_.size());
            ImGui::InputText("Thread parameter", threadParameter_.data(), threadParameter_.size());
            if (ImGui::Button("Create remote thread")) {
                if (auto start = parseAddress(threadStart_.data())) {
                    const auto param = parseAddress(threadParameter_.data()).value_or(0);
                    const auto startAddress = *start;
                    confirmAction(
                        "Run code in the target process?",
                        "This starts a thread at " + domain::toHex(startAddress) + " inside " +
                        domain::narrow(services_.session().processName()) +
                        ". If that address does not contain valid code the target will almost certainly crash.",
                        "Create thread",
                        [this, startAddress, param] {
                            auto result = services_.injector().createThread(startAddress, param);
                            if (result) {
                                notifyInfo("Remote thread finished with exit code " + std::to_string(result.value()) + ".");
                            } else {
                                notifyError("Remote thread failed: " + result.error());
                            }
                        });
                } else {
                    notifyError("Thread start address is not valid hexadecimal.");
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("LoadLibrary")) {
            ImGui::InputText("DLL path", dllPath_.data(), dllPath_.size());
            if (ImGui::Button("LoadLibraryW injection")) {
                const std::string path = dllPath_.data();
                if (path.empty()) {
                    notifyError("Enter the full path of the DLL to inject.");
                } else {
                    confirmAction(
                        "Inject a DLL into the target?",
                        "This loads\n\n" + path + "\n\ninto " +
                        domain::narrow(services_.session().processName()) +
                        ". The DLL runs with that process's privileges and cannot be unloaded by Pointer Lab.",
                        "Inject",
                        [this, path] {
                            auto result = services_.injector().loadLibrary(domain::widen(path));
                            if (result) {
                                notifyInfo("Injected. LoadLibraryW returned " + std::to_string(result.value()) + ".");
                            } else {
                                notifyError("LoadLibrary injection failed: " + result.error());
                            }
                        });
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiApp::renderLuaScannerPanel() {
    ImGui::Begin("Lua Scanner", &showLuaScanner_);
    const auto typeNames = valueTypeNames();

    ImGui::BeginChild("lua-scan-controls", ImVec2(0, 140.0f), true);
    statusPill(services_.luaScanJob().progress().running ? "RUNNING" : "READY",
        services_.luaScanJob().progress().running ? colorFromBytes(51, 94, 120) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    ImGui::TextDisabled("Return a Lua predicate: function(ctx) -> truthy to keep the address.");
    ImGui::SameLine();
    helpMarker("ctx fields: address, value, bytes, hex, type, region_base, region_size. Example: return function(ctx) return ctx.value and ctx.value % 16 == 0 end");
    ImGui::Separator();

    ImGui::SetNextItemWidth(118.0f);
    ImGui::Combo("Type", &luaScanTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("Stride", &luaScanStride_);
    luaScanStride_ = std::clamp(luaScanStride_, 0, 4096);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputInt("Max results", &luaScanMaxResults_);
    luaScanMaxResults_ = std::clamp(luaScanMaxResults_, 1, 1000000);

    ImGui::Checkbox("Writable only", &luaScanWritableOnly_);
    ImGui::SameLine();
    ImGui::Checkbox("Executable only", &luaScanExecutableOnly_);
    ImGui::SameLine();
    if (ImGui::Button("Start Lua scan")) {
        scripting::LuaScanOptions options;
        options.type = valueTypeFromIndex(luaScanTypeIndex_);
        options.script = luaScanScript_.data();
        options.stride = static_cast<std::size_t>(luaScanStride_);
        options.maxResults = static_cast<std::size_t>(luaScanMaxResults_);
        options.writableOnly = luaScanWritableOnly_;
        options.executableOnly = luaScanExecutableOnly_;
        services_.luaScanJob().start(std::move(options));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services_.luaScanJob().cancel();
    }
    ImGui::EndChild();

    if (ImGui::BeginTabBar("lua-scanner-tabs")) {
        if (ImGui::BeginTabItem("Predicate")) {
            ImGui::InputTextMultiline("##lua-scan-script", luaScanScript_.data(), luaScanScript_.size(), ImVec2(-1, -1));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Results")) {
            const auto progress = services_.luaScanJob().progress();
            ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
            if (!progress.error.empty()) {
                ImGui::TextColored(colorFromBytes(235, 116, 91), "%s", progress.error.c_str());
            } else {
                ImGui::TextDisabled("%zu Lua match%s", progress.results, progress.results == 1 ? "" : "es");
            }

            const auto results = services_.luaScanJob().results();
            const auto type = services_.luaScanJob().valueType();
            if (ImGui::BeginTable("lua-scan-results", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("Bytes");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < results.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::toHex(results[i].address).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::formatValue(type, results[i].current).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::bytesToHex(results[i].current).c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("Add")) {
                        services_.addressList().add(results[i].address, type, "Lua scan result", "Lua Scanner");
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("View")) {
                        copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(results[i].address));
                        copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(results[i].address));
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Examples")) {
            ImGui::TextWrapped("Lua Scanner scripts must return a predicate function. The predicate receives ctx and returns true to keep the address.");
            ImGui::Separator();
            ImGui::BulletText("i32 greater than 1000:");
            ImGui::TextUnformatted("return function(ctx)\n    return ctx.value and ctx.value > 1000\nend");
            ImGui::BulletText("aligned addresses only:");
            ImGui::TextUnformatted("return function(ctx)\n    return ctx.address % 16 == 0\nend");
            ImGui::BulletText("byte pattern check:");
            ImGui::TextUnformatted("return function(ctx)\n    return ctx.hex:sub(1, 4) == \"9090\"\nend");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiApp::renderLuaPanel() {
    ImGui::Begin("Lua Console", &showLuaConsole_);
    if (ImGui::CollapsingHeader("API quick reference")) {
        ImGui::TextWrapped(
            "Target:   processes(), attach(pid), detach(), modules(), regions()\n"
            "Memory:   read(addr [, type]), write(addr, type, value), read_u32(addr), write_u32(addr, value),\n"
            "          read_bytes(addr, n), write_bytes(addr, hex)\n"
            "Scanning: scan_exact(value [, type]), scan_unknown([type]), scan_next(mode [, value]),\n"
            "          scan_wait([ms]), scan_status(), scan_results([max]) -> table, total\n"
            "Pointers: resolve(module, offset, {offsets}) -> address\n"
            "Table:    add_address(addr [, type, description, group]) -> id\n"
            "Target code: alloc(size), thread(start [, param]), loadlibrary(path)\n"
            "\n"
            "Types are the same names the UI uses: i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 bytes.\n"
            "Scan modes: exact, unknown, changed, unchanged, increased, decreased.\n"
            "io, package, require, dofile, loadfile and the destructive half of os are removed.\n"
            "Full reference with return values and error behaviour: docs/lua-api.md in the repository.");
    }

    const bool running = lua_.running();
    const float available = ImGui::GetContentRegionAvail().y;
    const float buttonsRowH = ImGui::GetFrameHeightWithSpacing();
    const float splitH = std::max(60.0f, (available - buttonsRowH) * 0.5f);

    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    ImGui::InputTextMultiline("Lua", luaInput_.data(), luaInput_.size(), ImVec2(-1, splitH));
    ImGui::PopFont();

    ImGui::BeginDisabled(running);
    if (ImGui::Button("Run Lua")) {
        if (!lua_.submit(luaInput_.data())) {
            notifyError("A script is already running.");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // Scripts run on their own thread now, so an endless loop is something the
    // user can stop rather than a reason to kill the whole application.
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop")) {
        lua_.cancel();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Clear output")) {
        luaOutput_.clear();
    }
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("running...");
    }
    ImGui::BeginChild("lua-output", ImVec2(0, splitH), true);
    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    for (const auto& line : luaOutput_) {
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void UiApp::renderLogPanel() {
    ImGui::Begin("Logs", &showLogs_);

    auto& logger = infra::Logger::instance();
    static const char* levelNames[] = {"trace", "info", "warn", "error"};
    int level = static_cast<int>(logger.minimumLevel());
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Level", &level, levelNames, IM_ARRAYSIZE(levelNames))) {
        logger.setMinimumLevel(static_cast<infra::LogLevel>(level));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##logfilter", "filter", logFilter_.data(), logFilter_.size());

    if (ImGui::Button("Clear")) {
        logger.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open log folder")) {
        const auto folder = logger.path().parent_path();
        // Surfacing the folder beats telling the user a path they then have to
        // type out by hand.
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", logger.path().string().c_str());

    const std::string filter = logFilter_.data();
    const auto records = logger.snapshot();

    ImGui::BeginChild("log-scroll", ImVec2(0, 0), true);
    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    for (const auto& record : records) {
        if (!filter.empty() && record.message.find(filter) == std::string::npos) {
            continue;
        }
        ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (record.level == infra::LogLevel::Error) {
            colour = colorFromBytes(235, 116, 91);
        } else if (record.level == infra::LogLevel::Warning) {
            colour = colorFromBytes(232, 184, 92);
        }
        ImGui::TextColored(colour, "[%5u] [%s] %s", record.threadId,
                           infra::Logger::levelName(record.level), record.message.c_str());
    }
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

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
                  "This clears the address list. Anything not saved is lost.",
                  "New project",
                  [this] {
                      services_.session().addressList().replace({});
                      projectPath_.clear();
                      notifyInfo("Started a new project.");
                  });
}

bool UiApp::saveProjectTo(const std::filesystem::path& path, bool quiet) {
    storage::ProjectTable table;
    table.lastPid = services_.session().pid();
    table.lastProcessName = services_.session().processName();
    table.entries = services_.session().addressList().snapshot();

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
    services_.session().addressList().replace(std::move(loaded.value().entries));
    if (!quiet) {
        notifyInfo("Loaded " + std::to_string(count) + " entries from " + path.filename().string() + ".");
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
    if (services_.session().addressList().snapshot().empty()) {
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

void UiApp::requestDetach() {
    const auto liveBreakpoints = services_.breakpoints().breakpoints().size();
    auto detach = [this] {
        services_.breakpoints().detachDebugger();
        services_.session().detach();
        notifyInfo("Detached from the target process.");
    };

    if (liveBreakpoints == 0) {
        detach();
        return;
    }
    confirmAction(
        "Detach with active breakpoints?",
        std::to_string(liveBreakpoints) + " breakpoint(s) are still set. Detaching removes them and "
        "restores the original instruction bytes. If any byte cannot be restored the target will be "
        "left with a 0xCC trap and will most likely crash.",
        "Detach", detach);
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
        ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 0.0f), ImVec2(520.0f, FLT_MAX));

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

    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(640.0f, FLT_MAX));
    if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextUnformatted(pendingConfirm_->title.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::PushTextWrapPos(600.0f);
        ImGui::TextUnformatted(pendingConfirm_->message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        if (ImGui::Button(pendingConfirm_->confirmLabel.c_str(), ImVec2(180.0f, 0.0f))) {
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
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pendingConfirm_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void UiApp::renderAboutWindow() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 0.0f), ImVec2(720.0f, FLT_MAX));
    if (ImGui::Begin("About Pointer Lab", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::TextUnformatted(POINTERLAB_PRODUCT_NAME " " POINTERLAB_VERSION_STRING);
        ImGui::TextDisabled("Windows x64 user-mode memory research tool");
        ImGui::Separator();
        ImGui::PushTextWrapPos(680.0f);
        ImGui::TextUnformatted(
            "Pointer Lab is free software licensed under the GNU General Public License, "
            "version 2. It is GPL licensed because it statically links Keystone.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextUnformatted("Source code:");
        ImGui::TextDisabled(POINTERLAB_REPO_URL);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextUnformatted("Third-party components:");
        ImGui::BulletText("Dear ImGui - MIT");
        ImGui::BulletText("Lua 5.4 - MIT");
        ImGui::BulletText("Zydis - MIT");
        ImGui::BulletText("Keystone - GPLv2");
        ImGui::BulletText("Roboto and Cousine fonts - Apache 2.0");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("See LICENSE and THIRD_PARTY_NOTICES.md for full terms.");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void UiApp::renderHelpWindow() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(820.0f, FLT_MAX));
    if (ImGui::Begin("Help", &showHelp_, ImGuiWindowFlags_NoDocking)) {
        ImGui::PushTextWrapPos(780.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextUnformatted("Responsible use");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted(
            "Use Pointer Lab only on software you own or are authorised to analyse. Attaching to "
            "online games or other people's systems may breach their terms of service or the law "
            "where you live. Anti-cheat software commonly treats tools like this as an attack.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Access");
        ImGui::TextUnformatted(
            "Pointer Lab requests SeDebugPrivilege at startup. Without it, many processes can only "
            "be opened read-only: scanning still works, but writing, freezing, patching, breakpoints "
            "and injection all fail. Run as administrator for full access. The command bar shows "
            "READ-ONLY when access is limited.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Address input");
        ImGui::TextUnformatted(
            "Every address field is read as hexadecimal, with or without an 0x prefix. "
            "'140001000' and '0x140001000' are the same address.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Disassembler and assembler");
        ImGui::TextUnformatted(
            "The listing is decoded by Zydis and the assembler is Keystone, so the whole x86-64 "
            "instruction set is available in Intel syntax. Write one instruction per line; ';' and "
            "'//' begin a comment. Code is assembled at the address in the Address field, so relative "
            "jumps and calls resolve correctly.");
        ImGui::BulletText("A patch shorter than the code it overwrites is padded with nops, so the");
        ImGui::Indent();
        ImGui::TextUnformatted("target never resumes in the middle of an instruction.");
        ImGui::Unindent();
        ImGui::BulletText("For raw bytes use '.byte 0x90, 0x90' or the Memory panel's patch field.");
        ImGui::BulletText("Follow jumps and calls with the button beside a branch in the listing.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Breakpoints");
        ImGui::TextUnformatted(
            "Setting a breakpoint writes an int3 over the first byte at that address. When it is hit, "
            "Pointer Lab rewinds the thread, puts the original byte back, single-steps over it and "
            "re-arms behind it, so the target keeps running and the breakpoint keeps firing. Detaching "
            "restores every byte it wrote.");
        ImGui::BulletText("Breakpoints need a writable code page and full process access.");
        ImGui::BulletText("A breakpoint in a hot loop slows the target down noticeably; that is the");
        ImGui::Indent();
        ImGui::TextUnformatted("cost of a round trip to the debugger on every hit.");
        ImGui::Unindent();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Pointer chains");
        ImGui::TextUnformatted(
            "A chain added from the pointer scanner is stored as a module name plus an offset and a "
            "list of steps, never as a fixed address. Pointer Lab re-resolves it about twice a second, "
            "so the entry keeps tracking the value after the target restarts somewhere else. An entry "
            "whose chain stops resolving is shown as unresolved rather than reading a stale address.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Lua");
        ImGui::TextUnformatted(
            "Scripts run on a background thread and can be stopped with the Stop button, so a runaway "
            "loop no longer freezes the application. The standard library is trimmed: io, package, "
            "require, dofile, loadfile and the destructive half of os are removed, because a script "
            "pasted from the internet has no business touching your file system.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Keyboard");
        ImGui::BulletText("F1 - F12   Toggle freeze on the address list entry with that hotkey.");
        ImGui::TextDisabled(
            "Hotkeys are registered with Windows, so they fire while the target window is in the "
            "foreground - which is when you actually want them. Only keys assigned to an entry are "
            "registered, so Pointer Lab does not take F1-F12 away from everything else on the machine. "
            "A key another application already owns cannot be registered, and falls back to working "
            "only while Pointer Lab has focus.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Files");
        ImGui::BulletText("Projects are saved as .iretable files (File menu).");
        ImGui::BulletText("Logs, layout, session, settings and crash dumps live in %%LOCALAPPDATA%%\\PointerLab.");
        ImGui::BulletText("Scan options and which panels are open are remembered between runs.");

        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void UiApp::syncGlobalHotkeys() {
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
