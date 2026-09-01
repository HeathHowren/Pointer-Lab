#pragma once

#include "infra/Result.h"

#include <functional>
#include <string>

namespace ire::services {

// What something that is not the UI thread may ask the window to do.
//
// The scripting layer sits below the UI and must not include it, but the figures
// in a piece of writing about a tool have to be captured by something
// repeatable. A screenshot taken by hand is out of date the moment a panel is
// renamed and nobody finds out until a reader does. This interface is the seam:
// `scripting` depends on it, `ui` implements it and registers itself, and the
// direction of the dependency is unchanged.
//
// Every method here is called from a worker thread -- the Lua script's, or the
// MCP server's -- and must not return until the UI thread has actually done the
// work. A screenshot that raced the frame it was meant to capture is worse than
// no screenshot, because it looks like a screenshot.
class UiCommands {
public:
    virtual ~UiCommands() = default;

    // Runs `work` on the UI thread and waits for it.
    //
    // This exists because the UI thread is the only thread that mutates engine
    // state. That was easy to hold to while the sole other caller was a capture
    // script driving panels, and stops being automatic the moment a second
    // client can attach, scan and patch on a thread of its own: two attaches
    // arriving at once leave the session half-built, and a detach racing a frame
    // pulls the handle out from under the panel drawing it. Anything that
    // changes what an engine holds goes through here; anything that only reads a
    // mutex-guarded snapshot does not need to.
    //
    // Failure is the request never being run -- the window went away, or it did
    // not answer in time. A failure *inside* `work` is the caller's to carry out
    // in whatever it captured.
    virtual infra::Result<void> runOnUiThread(std::function<void()> work) = 0;

    // The window's own back buffer, as a PNG. Not the screen: a window behind
    // this one, or a notification that happened to appear, must not end up in a
    // figure.
    virtual infra::Result<void> screenshot(const std::string& path) = 0;
    // Opens the named panel if it is closed and brings it to the front of its
    // tab bar. The name is the panel's title, exactly as it appears in the View
    // menu.
    virtual infra::Result<void> selectPanel(const std::string& name) = 0;
    // "default" restores the shipped arrangement, which is the only arrangement
    // a figure should ever be captured in.
    virtual infra::Result<void> setLayout(const std::string& name) = 0;
    // A fixed client size, so two figures captured a year apart are the same
    // number of pixels wide.
    virtual infra::Result<void> setWindowSize(int width, int height) = 0;
    // Lets that many frames be drawn before the script continues. Opening a
    // panel takes effect on the next frame and docking settles on the one after
    // it, so a capture that follows immediately catches the layout mid-move.
    virtual infra::Result<void> waitFrames(int frames) = 0;
    virtual infra::Result<void> quit() = 0;

    // Project files. These live here rather than being rebuilt by each caller
    // because saving a table is not only writing the engines' contents out: it
    // is also switching scripts off before they are dropped, re-resolving
    // symbols against the target attached now, and reporting a table built
    // against the other bitness. That is a page of decisions with a reason
    // behind each one, and a second copy of it would drift from this one.
    virtual infra::Result<void> saveProject(const std::string& path) = 0;
    virtual infra::Result<void> loadProject(const std::string& path) = 0;
};

// Null when nothing is driving a window -- the tests, and anything else that
// links the engines without the frontend. Scripts calling these functions then
// fail with a sentence saying so rather than dereferencing it.
[[nodiscard]] UiCommands* uiCommands();
void setUiCommands(UiCommands* commands);

} // namespace ire::services
