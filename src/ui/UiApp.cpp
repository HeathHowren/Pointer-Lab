#include "ui/UiApp.h"

#include "infra/Logger.h"
#include "infra/Paths.h"
#include "platform_win32/Win32Platform.h"

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

std::vector<const char*> valueTypeNames() {
    std::vector<const char*> names;
    for (const auto type : domain::valueTypes()) {
        names.push_back(domain::valueTypeName(type));
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

storage::ProjectTable tableFromSession(domain::TargetSession& session) {
    storage::ProjectTable table;
    table.lastPid = session.pid();
    table.lastProcessName = session.processName();
    table.entries = session.addressList().snapshot();
    return table;
}

} // namespace

UiApp::UiApp(HINSTANCE instance, int showCommand)
    : instance_(instance), showCommand_(showCommand), lua_(services_) {
    copyText(projectPath_.data(), projectPath_.size(), infra::Paths::sessionFile().string());
    copyText(addDescription_.data(), addDescription_.size(), "Manual entry");
    copyText(addGroup_.data(), addGroup_.size(), "Default");
    copyText(luaScanScript_.data(), luaScanScript_.size(),
        "return function(ctx)\n"
        "    -- ctx.address, ctx.value, ctx.bytes, ctx.hex, ctx.type\n"
        "    return ctx.value ~= nil and ctx.value > 1000\n"
        "end\n");
}

UiApp::~UiApp() {
    saveProject(infra::Paths::sessionFile());
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
    UnregisterClassW(L"PointerLabWindow", instance_);
}

int UiApp::run() {
    if (!createWindow()) {
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
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
    loadProject(infra::Paths::sessionFile());

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
        render();
    }

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
        if (device_ != nullptr && wParam != SIZE_MINIMIZED) {
            cleanupRenderTarget();
            swapChain_->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
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
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowW(wc.lpszClassName, L"Pointer Lab", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1500, 950, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }
    if (!createDeviceD3D(hwnd_)) {
        cleanupDeviceD3D();
        return false;
    }
    return true;
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
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    device_->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView_);
    if (backBuffer) {
        backBuffer->Release();
    }
}

void UiApp::cleanupRenderTarget() {
    if (renderTargetView_) {
        renderTargetView_->Release();
        renderTargetView_ = nullptr;
    }
}

void UiApp::render() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    handleHotkeys();
    renderMenu();
    renderCommandBar();
    renderDockspace();
    renderProcessPanel();
    renderScanPanel();
    renderAddressListPanel();
    renderMemoryPanel();
    renderDisassemblyPanel();
    renderBreakpointPanel();
    renderModulesPanel();
    renderRegionsPanel();
    renderPointerPanel();
    renderInjectionPanel();
    renderLuaScannerPanel();
    renderLuaPanel();
    renderLogPanel();

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

    swapChain_->Present(1, 0);
}

void UiApp::applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.WindowRounding = 9.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 7.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg] = colorFromBytes(18, 22, 27);
    colors[ImGuiCol_ChildBg] = colorFromBytes(22, 27, 33);
    colors[ImGuiCol_PopupBg] = colorFromBytes(25, 30, 37);
    colors[ImGuiCol_Border] = colorFromBytes(55, 65, 75);
    colors[ImGuiCol_FrameBg] = colorFromBytes(31, 38, 47);
    colors[ImGuiCol_FrameBgHovered] = colorFromBytes(41, 52, 63);
    colors[ImGuiCol_FrameBgActive] = colorFromBytes(50, 63, 76);
    colors[ImGuiCol_TitleBg] = colorFromBytes(15, 19, 24);
    colors[ImGuiCol_TitleBgActive] = colorFromBytes(28, 36, 44);
    colors[ImGuiCol_MenuBarBg] = colorFromBytes(13, 17, 21);
    colors[ImGuiCol_Button] = colorFromBytes(39, 57, 68);
    colors[ImGuiCol_ButtonHovered] = colorFromBytes(54, 79, 94);
    colors[ImGuiCol_ButtonActive] = colorFromBytes(69, 98, 113);
    colors[ImGuiCol_Header] = colorFromBytes(35, 51, 61);
    colors[ImGuiCol_HeaderHovered] = colorFromBytes(50, 73, 86);
    colors[ImGuiCol_HeaderActive] = colorFromBytes(64, 92, 105);
    colors[ImGuiCol_Tab] = colorFromBytes(24, 30, 37);
    colors[ImGuiCol_TabHovered] = colorFromBytes(61, 91, 104);
    colors[ImGuiCol_TabSelected] = colorFromBytes(39, 56, 66);
    colors[ImGuiCol_TabDimmed] = colorFromBytes(18, 23, 28);
    colors[ImGuiCol_TabDimmedSelected] = colorFromBytes(30, 40, 49);
    colors[ImGuiCol_DockingPreview] = colorFromBytes(92, 166, 184, 180);
    colors[ImGuiCol_CheckMark] = colorFromBytes(114, 211, 189);
    colors[ImGuiCol_SliderGrab] = colorFromBytes(101, 177, 194);
    colors[ImGuiCol_SliderGrabActive] = colorFromBytes(126, 214, 225);
    colors[ImGuiCol_ResizeGrip] = colorFromBytes(61, 91, 104, 90);
    colors[ImGuiCol_ResizeGripHovered] = colorFromBytes(92, 166, 184, 160);
    colors[ImGuiCol_Text] = colorFromBytes(227, 233, 238);
    colors[ImGuiCol_TextDisabled] = colorFromBytes(137, 149, 158);

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
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
        buildDefaultDockLayout(dockspaceId, viewport->WorkSize);
        resetDockLayout_ = false;
    }
    dockLayoutInitialized_ = true;
}

void UiApp::buildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);

    ImGuiID left{};
    ImGuiID right{};
    ImGuiID bottom{};
    ImGuiID center{};
    ImGuiID centerBottom{};
    ImGuiID centerTop{};

    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.22f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.34f, &bottom, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.38f, &centerBottom, &centerTop);

    ImGui::DockBuilderDockWindow("Process Selection", left);
    ImGui::DockBuilderDockWindow("Modules", left);
    ImGui::DockBuilderDockWindow("Memory Regions", left);

    ImGui::DockBuilderDockWindow("Scanner", centerTop);
    ImGui::DockBuilderDockWindow("Address List", centerBottom);

    ImGui::DockBuilderDockWindow("Memory Viewer", right);
    ImGui::DockBuilderDockWindow("Disassembly", right);
    ImGui::DockBuilderDockWindow("Breakpoints", right);

    ImGui::DockBuilderDockWindow("Pointer Scanner", bottom);
    ImGui::DockBuilderDockWindow("Lua Scanner", bottom);
    ImGui::DockBuilderDockWindow("Injection", bottom);
    ImGui::DockBuilderDockWindow("Lua Console", bottom);
    ImGui::DockBuilderDockWindow("Logs", bottom);

    ImGui::DockBuilderFinish(dockspaceId);
    infra::Logger::instance().info("Applied default IDE layout.");
}

void UiApp::renderMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("Project path", projectPath_.data(), projectPath_.size());
        if (ImGui::MenuItem("Save")) {
            saveProject(projectPath_.data());
        }
        if (ImGui::MenuItem("Load")) {
            loadProject(projectPath_.data());
        }
        if (ImGui::MenuItem("Save session")) {
            saveProject(infra::Paths::sessionFile());
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset default layout")) {
            resetDockLayout_ = true;
        }
        ImGui::MenuItem("Show manual address editor", nullptr, &showManualAddressEditor_);
        ImGui::MenuItem("Show scan filters", nullptr, &showScannerFilters_);
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
            services_.breakpoints().detachDebugger();
            services_.session().detach();
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
    }

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20.0f, 0.0f));
    ImGui::SameLine();
    const auto scanProgress = services_.scanJob().progress();
    statusPill(scanProgress.running ? "SCAN RUNNING" : "SCAN IDLE", scanProgress.running ? colorFromBytes(51, 94, 120) : colorFromBytes(50, 63, 76));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu results", scanProgress.results);

    ImGui::SameLine(ImGui::GetWindowWidth() - 470.0f);
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
    if (ImGui::SmallButton("Save Session")) {
        saveProject(infra::Paths::sessionFile());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Detach") && attached) {
        services_.breakpoints().detachDebugger();
        services_.session().detach();
    }

    ImGui::End();
}

void UiApp::renderProcessPanel() {
    ImGui::Begin("Process Selection");
    statusPill(services_.session().attached() ? "CONNECTED" : "BROWSE", services_.session().attached() ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu processes", processes_.size());
    ImGui::Separator();

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
    float reservedWidth = ImGui::CalcTextSize("Refresh").x + pad + spacing;
    if (services_.session().attached())
        reservedWidth += ImGui::CalcTextSize("Detach").x + pad + spacing;
    ImGui::SetNextItemWidth(-reservedWidth);
    ImGui::InputText("Filter", processFilter_.data(), processFilter_.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        refreshProcesses();
    }
    if (services_.session().attached()) {
        ImGui::SameLine();
        if (ImGui::Button("Detach")) {
            services_.breakpoints().detachDebugger();
            services_.session().detach();
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
                    infra::Logger::instance().error("Attach failed: " + result.error());
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

    ImGui::BeginChild("scan-controls", ImVec2(0, 112.0f), true);
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
    auto scanValue = domain::parseScanValue(type, scanText_.data());
    if (!scanValue && scanModeFromIndex(scanModeIndex_) == domain::ScanMode::UnknownInitial && type != domain::ValueType::Bytes) {
        scanValue = domain::parseScanValue(type, "0");
    }

    if (ImGui::Button("First scan")) {
        if (scanValue) {
            engine_scan::ScanOptions options;
            options.writableOnly = scanWritableOnly_;
            options.executableOnly = scanExecutableOnly_;
            services_.scanJob().setOptions(options);
            services_.scanJob().startFirst(scanModeFromIndex(scanModeIndex_), *scanValue);
        } else {
            infra::Logger::instance().warn("Scan value is invalid.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next scan")) {
        if (scanValue) {
            engine_scan::ScanOptions options;
            options.writableOnly = scanWritableOnly_;
            options.executableOnly = scanExecutableOnly_;
            services_.scanJob().setOptions(options);
            services_.scanJob().startNext(scanModeFromIndex(scanModeIndex_), *scanValue, services_.scanJob().results());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services_.scanJob().cancel();
    }

    const auto progress = services_.scanJob().progress();
    ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
    ImGui::TextDisabled("%zu result%s visible in the current result set", progress.results, progress.results == 1 ? "" : "s");

    auto results = services_.scanJob().results();
    if (ImGui::BeginTable("scan-results", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Previous");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        const std::size_t count = std::min<std::size_t>(results.size(), 10000);
        for (std::size_t i = 0; i < count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(results[i].address).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::formatValue(type, results[i].previous).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::formatValue(type, results[i].current).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Add")) {
                services_.addressList().add(results[i].address, type, "Scan result", "Scan");
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
            ImGui::TextUnformatted(domain::toHex(entry.address).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::valueTypeName(entry.type));
            ImGui::TableNextColumn();
            std::string current = "<unreadable>";
            if (services_.session().attached()) {
                if (auto bytes = services_.session().readBytes(entry.address, std::max<std::size_t>(1, domain::valueTypeSize(entry.type)))) {
                    current = domain::formatValue(entry.type, bytes.value());
                }
            }
            ImGui::TextUnformatted(current.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(entry.id));
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
            if (ImGui::SmallButton("Write")) {
                if (auto value = domain::parseScanValue(entry.type, addValue_.data())) {
                    services_.addressList().updateValue(entry.id, value->bytes);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                services_.addressList().remove(entry.id);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderMemoryPanel() {
    ImGui::Begin("Memory Viewer");
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
                    if (!patch.empty()) {
                        if (auto result = services_.session().writeBytes(*address, patch); !result) {
                            infra::Logger::instance().error("Patch failed: " + result.error());
                        }
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiApp::renderDisassemblyPanel() {
    ImGui::Begin("Disassembly");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Address", "0x7FF...", disasmAddress_.data(), disasmAddress_.size());
    const auto address = parseAddress(disasmAddress_.data());
    if (ImGui::BeginTabBar("disasm-tabs")) {
        if (ImGui::BeginTabItem("Listing")) {
            if (address && services_.session().attached()) {
                auto instructions = services_.disassembler().disassemble(services_.session(), *address, 64);
                if (ImGui::BeginTable("disasm", 3, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                    ImGui::TableSetupColumn("Address");
                    ImGui::TableSetupColumn("Bytes");
                    ImGui::TableSetupColumn("Instruction");
                    ImGui::TableHeadersRow();
                    for (const auto& ins : instructions) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(domain::toHex(ins.address).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(domain::bytesToHex(ins.bytes).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(ins.text.c_str());
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::TextDisabled("Attach and enter an address to disassemble around it.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Assembler Patch")) {
            ImGui::TextDisabled("Supported: nop, ret, int3, xor eax/eax, db, jmp, call, push imm32, mov rax, imm64.");
            ImGui::InputTextMultiline("##Assembler", assemblerText_.data(), assemblerText_.size(), ImVec2(-1, -34.0f));
            if (ImGui::Button("Assemble and patch")) {
                if (address) {
                    auto assembled = services_.assembler().assemble(assemblerText_.data(), *address);
                    if (!assembled) {
                        infra::Logger::instance().error("Assemble failed: " + assembled.error());
                    } else if (auto result = services_.session().writeBytes(*address, assembled.value()); !result) {
                        infra::Logger::instance().error("Patch failed: " + result.error());
                    } else {
                        infra::Logger::instance().info("Patched " + std::to_string(assembled.value().size()) + " assembled bytes.");
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
    ImGui::Begin("Breakpoints");
    statusPill(services_.breakpoints().debuggerAttached() ? "DEBUGGER ATTACHED" : "DEBUGGER OFF",
        services_.breakpoints().debuggerAttached() ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    if (ImGui::Button(services_.breakpoints().debuggerAttached() ? "Detach debugger" : "Attach debugger")) {
        if (services_.breakpoints().debuggerAttached()) {
            services_.breakpoints().detachDebugger();
        } else if (auto result = services_.breakpoints().attachDebugger(); !result) {
            infra::Logger::instance().error("Debugger attach failed: " + result.error());
        }
    }
    ImGui::SetNextItemWidth(210.0f);
    ImGui::InputTextWithHint("Address", "0x7FF...", breakpointAddress_.data(), breakpointAddress_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputTextWithHint("Label", "optional name", breakpointLabel_.data(), breakpointLabel_.size());
    ImGui::SameLine();
    if (ImGui::Button("Set software breakpoint")) {
        if (auto address = parseAddress(breakpointAddress_.data())) {
            if (auto result = services_.breakpoints().addBreakpoint(*address, breakpointLabel_.data()); !result) {
                infra::Logger::instance().error("Breakpoint failed: " + result.error());
            }
        }
    }
    if (ImGui::BeginTable("breakpoints", 5, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Label");
        ImGui::TableSetupColumn("Enabled");
        ImGui::TableSetupColumn("Hits");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& bp : services_.breakpoints().breakpoints()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(bp.address).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bp.label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bp.enabled ? "yes" : "no");
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(bp.hitCount));
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(bp.address));
            if (ImGui::SmallButton("Remove")) {
                services_.breakpoints().removeBreakpoint(bp.address);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderModulesPanel() {
    ImGui::Begin("Modules");
    if (ImGui::Button("Refresh") && services_.session().attached()) {
        services_.session().refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu loaded", services_.session().modules().size());
    if (ImGui::BeginTable("modules", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Base");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Path");
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
    ImGui::Begin("Memory Regions");
    if (ImGui::Button("Refresh") && services_.session().attached()) {
        services_.session().refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu regions", services_.session().regions().size());
    if (ImGui::BeginTable("regions", 6, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Base");
        ImGui::TableSetupColumn("End");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Protect");
        ImGui::TableSetupColumn("Access");
        ImGui::TableSetupColumn("Action");
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
            ImGui::PushID(static_cast<int>(region.base));
            if (ImGui::SmallButton("View")) {
                copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(region.base));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderPointerPanel() {
    ImGui::Begin("Pointer Scanner");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Target address", "0x7FF...", pointerTarget_.data(), pointerTarget_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("Max depth", &pointerDepth_);
    pointerDepth_ = std::clamp(pointerDepth_, 1, 8);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Max offset", pointerMaxOffset_.data(), pointerMaxOffset_.size());
    if (ImGui::Button("Start pointer scan")) {
        if (auto target = parseAddress(pointerTarget_.data())) {
            engine_pointer::PointerScanOptions options;
            options.target = *target;
            options.maxDepth = static_cast<std::uint32_t>(pointerDepth_);
            options.maxOffset = static_cast<std::uint32_t>(parseAddress(pointerMaxOffset_.data()).value_or(0x1000));
            services_.pointerScanJob().start(options);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel pointer scan")) {
        services_.pointerScanJob().cancel();
    }
    const auto progress = services_.pointerScanJob().progress();
    ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());

    if (ImGui::BeginTable("pointer-results", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Module");
        ImGui::TableSetupColumn("Base address");
        ImGui::TableSetupColumn("Offsets");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& chain : services_.pointerScanJob().results()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::narrow(chain.moduleName).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::toHex(chain.baseAddress).c_str());
            ImGui::TableNextColumn();
            std::ostringstream offsets;
            for (std::size_t i = 0; i < chain.offsets.size(); ++i) {
                if (i) offsets << ", ";
                offsets << "0x" << std::hex << chain.offsets[i];
            }
            ImGui::TextUnformatted(offsets.str().c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Add base")) {
                services_.addressList().add(chain.baseAddress, domain::ValueType::UInt64, "Pointer base", "Pointers");
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void UiApp::renderInjectionPanel() {
    ImGui::Begin("Injection");
    if (ImGui::BeginTabBar("inject-tabs")) {
        if (ImGui::BeginTabItem("Allocate")) {
            ImGui::InputText("Allocation size", allocSize_.data(), allocSize_.size());
            if (ImGui::Button("Remote allocate RWX")) {
                const auto size = static_cast<std::size_t>(parseAddress(allocSize_.data()).value_or(4096));
                auto result = services_.injector().allocate(size, PAGE_EXECUTE_READWRITE);
                if (result) {
                    infra::Logger::instance().info("Remote allocation: " + domain::toHex(result.value()));
                    copyText(threadStart_.data(), threadStart_.size(), domain::toHex(result.value()));
                } else {
                    infra::Logger::instance().error("Allocation failed: " + result.error());
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Remote Thread")) {
            ImGui::InputText("Thread start", threadStart_.data(), threadStart_.size());
            ImGui::InputText("Thread parameter", threadParameter_.data(), threadParameter_.size());
            if (ImGui::Button("Create remote thread")) {
                if (auto start = parseAddress(threadStart_.data())) {
                    auto param = parseAddress(threadParameter_.data()).value_or(0);
                    auto result = services_.injector().createThread(*start, param);
                    if (!result) {
                        infra::Logger::instance().error("Remote thread failed: " + result.error());
                    } else {
                        infra::Logger::instance().info("Remote thread exit code: " + std::to_string(result.value()));
                    }
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("LoadLibrary")) {
            ImGui::InputText("DLL path", dllPath_.data(), dllPath_.size());
            if (ImGui::Button("LoadLibraryW injection")) {
                auto result = services_.injector().loadLibrary(domain::widen(dllPath_.data()));
                if (!result) {
                    infra::Logger::instance().error("LoadLibrary injection failed: " + result.error());
                } else {
                    infra::Logger::instance().info("LoadLibrary remote thread exit code: " + std::to_string(result.value()));
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiApp::renderLuaScannerPanel() {
    ImGui::Begin("Lua Scanner");
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
    ImGui::Begin("Lua Console");
    if (ImGui::CollapsingHeader("API quick reference")) {
        ImGui::TextWrapped("processes(), attach(pid), detach(), read_u32(addr), write_u32(addr,val), read_bytes(addr,n), write_bytes(addr,hex), scan_exact_i32(v), scan_unknown_i32(), add_address(addr,type,desc,group), alloc(size), thread(start,param), loadlibrary(path)");
    }
    ImGui::InputTextMultiline("Lua", luaInput_.data(), luaInput_.size(), ImVec2(-1, 170));
    if (ImGui::Button("Run Lua")) {
        const auto output = lua_.run(luaInput_.data());
        luaOutput_.insert(luaOutput_.end(), output.begin(), output.end());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear output")) {
        luaOutput_.clear();
    }
    ImGui::BeginChild("lua-output", ImVec2(0, 180), true);
    for (const auto& line : luaOutput_) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void UiApp::renderLogPanel() {
    ImGui::Begin("Logs");
    ImGui::TextDisabled("Runtime log");
    ImGui::BeginChild("log-scroll", ImVec2(0, 0), true);
    for (const auto& record : infra::Logger::instance().snapshot()) {
        ImGui::Text("[%s] %s", infra::Logger::levelName(record.level), record.message.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void UiApp::handleHotkeys() {
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    const ImGuiKey keys[] = {ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12};
    for (int i = 0; i < IM_ARRAYSIZE(keys); ++i) {
        if (ImGui::IsKeyPressed(keys[i], false)) {
            services_.addressList().toggleHotkey("F" + std::to_string(i + 1));
        }
    }
}

void UiApp::refreshProcesses() {
    processes_ = services_.platform().listProcesses();
}

void UiApp::saveProject(const std::filesystem::path& path) {
    if (auto result = projectStore_.save(path, tableFromSession(services_.session())); !result) {
        infra::Logger::instance().error("Save failed: " + result.error());
    } else {
        infra::Logger::instance().info("Saved project to " + path.string() + ".");
    }
}

void UiApp::loadProject(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto loaded = projectStore_.load(path);
    if (!loaded) {
        infra::Logger::instance().warn("Load skipped: " + loaded.error());
        return;
    }
    services_.session().addressList().replace(loaded.value().entries);
    infra::Logger::instance().info("Loaded project table from " + path.string() + ".");
}

std::optional<std::uintptr_t> UiApp::parseAddress(const char* text) {
    if (!text || text[0] == '\0') {
        return std::nullopt;
    }
    try {
        std::string value(text);
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; }), value.end());
        int base = 10;
        if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
            base = 16;
        } else if (value.find_first_of("ABCDEFabcdef") != std::string::npos) {
            base = 16;
        }
        return static_cast<std::uintptr_t>(std::stoull(value, nullptr, base));
    } catch (...) {
        return std::nullopt;
    }
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
