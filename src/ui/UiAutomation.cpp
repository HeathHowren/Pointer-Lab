// Scripted control of the window, so that a set of figures can be captured by
// running a file rather than by a person with a screenshot key.
//
// The reason this exists is not convenience. A figure captured by hand is
// correct on the day it was taken and silently wrong afterwards: a panel gets
// renamed, a button moves, and nothing tells anybody until a reader follows an
// instruction that no longer matches the picture beside it. A capture script is
// re-run on every release, and a panel that no longer exists fails loudly.
//
// Two things make a capture deterministic, and both are here because neither is
// obvious. The window is given an exact client size, so two figures taken a year
// apart are the same number of pixels. And every request takes effect on a frame
// boundary the script can wait for, because opening a panel is visible on the
// next frame and its docking settles on the one after that -- a capture that
// followed immediately would catch the layout mid-move.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "infra/Logger.h"

#include <wincodec.h>

#include <chrono>
#include <cstring>
#include <exception>

namespace ire::ui {

namespace {

template <typename T>
using Result = infra::Result<T>;

// Long enough that a slow frame or a resize never trips it, short enough that a
// script does not hang forever if the window has gone away.
constexpr auto requestTimeout = std::chrono::seconds(20);

template <typename T>
void release(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The script side: queue a request, wait for the UI thread to run it
// ---------------------------------------------------------------------------

infra::Result<void> UiApp::submitRequest(std::shared_ptr<AutomationRequest> request) {
    // A request submitted from the UI thread itself would wait for a drain that
    // cannot run until it returns. Nothing does that today, but the failure is a
    // silently hung window rather than an error, so it is checked rather than
    // relied upon.
    if (std::this_thread::get_id() == uiThreadId_) {
        return Result<void>::fail("A window request cannot be made from the UI thread itself.");
    }

    std::unique_lock lock(automationMutex_);
    automationQueue_.push_back(request);
    const bool ran = automationDone_.wait_for(lock, requestTimeout, [&request] { return request->finished; });
    if (!ran) {
        // The request is deliberately left in the queue: it may still run, and
        // removing it from under a drain in progress is worse than a late one.
        return Result<void>::fail("The window did not respond within 20 seconds.");
    }
    if (!request->ok) {
        return Result<void>::fail(request->error);
    }
    return Result<void>::ok();
}

infra::Result<void> UiApp::automationRequest(AutomationRequest::Kind kind, std::string text, int first,
                                             int second) {
    auto request = std::make_shared<AutomationRequest>();
    request->kind = kind;
    request->text = std::move(text);
    request->first = first;
    request->second = second;
    return submitRequest(std::move(request));
}

infra::Result<void> UiApp::runOnUiThread(std::function<void()> work) {
    if (!work) {
        return Result<void>::ok();
    }
    auto request = std::make_shared<AutomationRequest>();
    request->kind = AutomationRequest::Kind::Invoke;
    request->work = std::move(work);
    return submitRequest(std::move(request));
}

infra::Result<void> UiApp::saveProject(const std::string& path) {
    auto outcome = Result<void>::fail("The project was not saved.");
    // Not quiet: the person at the window should see that their project was
    // just written, and by what.
    if (auto ran = runOnUiThread([&] { outcome = saveProjectTo(std::filesystem::path(path), false); }); !ran) {
        return ran;
    }
    return outcome;
}

infra::Result<void> UiApp::loadProject(const std::string& path) {
    auto outcome = Result<void>::fail("The project was not loaded.");
    if (auto ran = runOnUiThread([&] {
            outcome = loadProjectFrom(std::filesystem::path(path), false);
            if (outcome) {
                projectPath_ = std::filesystem::path(path);
            }
        });
        !ran) {
        return ran;
    }
    return outcome;
}

infra::Result<void> UiApp::screenshot(const std::string& path) {
    return automationRequest(AutomationRequest::Kind::Screenshot, path);
}

infra::Result<void> UiApp::selectPanel(const std::string& name) {
    return automationRequest(AutomationRequest::Kind::SelectPanel, name);
}

infra::Result<void> UiApp::setLayout(const std::string& name) {
    return automationRequest(AutomationRequest::Kind::SetLayout, name);
}

infra::Result<void> UiApp::setWindowSize(int width, int height) {
    if (width < 320 || height < 240 || width > 8192 || height > 8192) {
        return Result<void>::fail("A window size has to be between 320x240 and 8192x8192.");
    }
    return automationRequest(AutomationRequest::Kind::SetWindowSize, {}, width, height);
}

infra::Result<void> UiApp::waitFrames(int frames) {
    if (frames < 1 || frames > 600) {
        return Result<void>::fail("Wait for between 1 and 600 frames.");
    }
    return automationRequest(AutomationRequest::Kind::WaitFrames, {}, frames);
}

infra::Result<void> UiApp::quit() {
    return automationRequest(AutomationRequest::Kind::Quit);
}

// ---------------------------------------------------------------------------
// The UI side
// ---------------------------------------------------------------------------

bool* UiApp::panelFlag(const std::string& name) {
    // Titles exactly as they are passed to ImGui::Begin, which is also exactly
    // how they read in the View menu. A script naming a panel that does not
    // exist is told so, which is the point: that is how a renamed panel is
    // discovered by the build rather than by a reader.
    if (name == "Memory Viewer")    { return &showMemoryViewer_; }
    if (name == "Disassembly")      { return &showDisassembly_; }
    if (name == "Breakpoints")      { return &showBreakpoints_; }
    if (name == "Access Watch")     { return &showAccessWatch_; }
    if (name == "Patches")          { return &showPatches_; }
    if (name == "Symbols")          { return &showSymbols_; }
    if (name == "Scripts")          { return &showScripts_; }
    if (name == "Structures")       { return &showStructures_; }
    if (name == "Speed and Export") { return &showSpeed_; }
    if (name == "Modules")          { return &showModules_; }
    if (name == "Memory Regions")   { return &showMemoryRegions_; }
    if (name == "Logs")             { return &showLogs_; }
    if (name == "Pointer Scanner")  { return &showPointerScanner_; }
    if (name == "Lua Scanner")      { return &showLuaScanner_; }
    if (name == "Injection")        { return &showInjection_; }
    if (name == "Lua Console")      { return &showLuaConsole_; }
    if (name == "MCP Server")       { return &showMcp_; }
    return nullptr;
}

void UiApp::drainAutomation() {
    std::vector<std::shared_ptr<AutomationRequest>> ready;
    {
        std::scoped_lock lock(automationMutex_);
        if (automationQueue_.empty()) {
            return;
        }
        // Screenshots stay behind for drainScreenshots(); everything else is
        // taken in order.
        for (auto& request : automationQueue_) {
            if (request->kind != AutomationRequest::Kind::Screenshot) {
                ready.push_back(request);
            }
        }
        std::erase_if(automationQueue_, [](const std::shared_ptr<AutomationRequest>& request) {
            return request->kind != AutomationRequest::Kind::Screenshot;
        });
    }

    for (auto& request : ready) {
        switch (request->kind) {
        case AutomationRequest::Kind::SelectPanel: {
            // The three core panels are always open, so a name that is not in
            // the map is only an error if it is not one of those either.
            if (bool* flag = panelFlag(request->text); flag != nullptr) {
                *flag = true;
            } else if (request->text != "Process Selection" && request->text != "Scanner" &&
                       request->text != "Address List") {
                request->ok = false;
                request->error = "There is no panel called \"" + request->text + "\".";
                break;
            }
            focusPanel_ = request->text;
            break;
        }
        case AutomationRequest::Kind::SetLayout:
            if (request->text != "default") {
                request->ok = false;
                request->error = "The only layout with a name is \"default\".";
                break;
            }
            resetDockLayout_ = true;
            break;
        case AutomationRequest::Kind::SetWindowSize: {
            RECT wanted{0, 0, request->first, request->second};
            // The requested size is the *client* area, because that is what
            // ends up in the picture; the frame around it differs between
            // Windows versions and themes.
            AdjustWindowRect(&wanted, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE);
            if (SetWindowPos(hwnd_, nullptr, 0, 0, wanted.right - wanted.left,
                             wanted.bottom - wanted.top, SWP_NOMOVE | SWP_NOZORDER) == 0) {
                request->ok = false;
                request->error = "The window could not be resized.";
            }
            break;
        }
        case AutomationRequest::Kind::WaitFrames:
            // Not finished until it has counted down, which is what makes this
            // a wait rather than a no-op.
            if (--request->first > 0) {
                std::scoped_lock lock(automationMutex_);
                automationQueue_.push_back(request);
                continue;
            }
            break;
        case AutomationRequest::Kind::Quit:
            quitRequested_ = true;
            break;
        case AutomationRequest::Kind::Invoke:
            // The work reports through whatever it captured, so there is nothing
            // to record here. It is wrapped because a handler that threw would
            // otherwise unwind through the frame loop and take the window with
            // it -- and the thread waiting on this request would wait out its
            // full timeout for an answer that is never coming.
            try {
                request->work();
            } catch (const std::exception& error) {
                request->ok = false;
                request->error = error.what();
            } catch (...) {
                request->ok = false;
                request->error = "The request threw an unknown exception.";
            }
            break;
        case AutomationRequest::Kind::Screenshot:
            break;
        }

        {
            std::scoped_lock lock(automationMutex_);
            request->finished = true;
        }
    }
    automationDone_.notify_all();
}

void UiApp::drainScreenshots() {
    std::vector<std::shared_ptr<AutomationRequest>> ready;
    {
        std::scoped_lock lock(automationMutex_);
        for (auto& request : automationQueue_) {
            if (request->kind == AutomationRequest::Kind::Screenshot) {
                ready.push_back(request);
            }
        }
        if (ready.empty()) {
            return;
        }
        std::erase_if(automationQueue_, [](const std::shared_ptr<AutomationRequest>& request) {
            return request->kind == AutomationRequest::Kind::Screenshot;
        });
    }

    for (auto& request : ready) {
        if (auto captured = captureBackBuffer(request->text); !captured) {
            request->ok = false;
            request->error = captured.error();
        }
        std::scoped_lock lock(automationMutex_);
        request->finished = true;
    }
    automationDone_.notify_all();
}

infra::Result<void> UiApp::captureBackBuffer(const std::string& path) {
    if (swapChain_ == nullptr || device_ == nullptr || deviceContext_ == nullptr) {
        return Result<void>::fail("There is no Direct3D device to capture from.");
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr) {
        return Result<void>::fail("The swap chain's back buffer could not be obtained.");
    }

    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);

    // A staging copy, because the back buffer itself cannot be mapped: it lives
    // where the GPU wants it and the CPU has no way in.
    D3D11_TEXTURE2D_DESC staging = description;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ID3D11Texture2D* copy = nullptr;
    if (FAILED(device_->CreateTexture2D(&staging, nullptr, &copy)) || copy == nullptr) {
        release(backBuffer);
        return Result<void>::fail("A readable copy of the back buffer could not be created.");
    }
    deviceContext_->CopyResource(copy, backBuffer);
    release(backBuffer);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Map blocks until the GPU has finished the copy, which is the only
    // synchronisation this needs: the draw calls for this frame were issued
    // before it.
    if (FAILED(deviceContext_->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))) {
        release(copy);
        return Result<void>::fail("The copied back buffer could not be read.");
    }

    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgba) {
        deviceContext_->Unmap(copy, 0);
        release(copy);
        return Result<void>::fail("The back buffer is in a pixel format this capture does not "
                                  "understand.");
    }

    const auto width = static_cast<std::size_t>(description.Width);
    const auto height = static_cast<std::size_t>(description.Height);
    std::vector<std::uint8_t> pixels(width * height * 4);
    for (std::size_t y = 0; y < height; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        std::uint8_t* destination = pixels.data() + y * width * 4;
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t* in = source + x * 4;
            std::uint8_t* out = destination + x * 4;
            out[0] = bgra ? in[0] : in[2];
            out[1] = in[1];
            out[2] = bgra ? in[2] : in[0];
            // Forced opaque. What was blended into the back buffer's alpha is an
            // artefact of how the panels were drawn, and a figure of a window
            // with semi-transparent regions in it is not a figure of a window.
            out[3] = 0xFF;
        }
    }
    deviceContext_->Unmap(copy, 0);
    release(copy);

    std::error_code ec;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }

    // COM may or may not already be initialised on this thread depending on what
    // else has run, so this both initialises it and remembers whether it has to
    // undo that.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = com == S_OK || com == S_FALSE;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    std::string failure;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        failure = "The Windows imaging component is unavailable.";
    } else if (FAILED(factory->CreateStream(&stream)) ||
               FAILED(stream->InitializeFromFilename(file.wstring().c_str(), GENERIC_WRITE))) {
        failure = "Could not open " + file.string() + " for writing.";
    } else if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
               FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) ||
               FAILED(encoder->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options))) {
        failure = "The PNG encoder could not be set up.";
    } else {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetSize(description.Width, description.Height)) ||
            FAILED(frame->SetPixelFormat(&format)) ||
            FAILED(frame->WritePixels(description.Height, static_cast<UINT>(width * 4),
                                      static_cast<UINT>(pixels.size()), pixels.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            failure = "The PNG could not be written to " + file.string() + ".";
        }
    }

    release(options);
    release(frame);
    release(encoder);
    release(stream);
    release(factory);
    if (uninitialize) {
        CoUninitialize();
    }

    if (!failure.empty()) {
        return Result<void>::fail(failure);
    }
    infra::Logger::instance().info("Captured " + file.string() + " (" + std::to_string(width) + "x" +
                                   std::to_string(height) + ").");
    return Result<void>::ok();
}

} // namespace ire::ui
