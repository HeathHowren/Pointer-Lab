#pragma once

#include "infra/Result.h"

#include <string>

namespace ire::services {

// What a script may ask the window to do.
//
// The scripting layer sits below the UI and must not include it, but the figures
// in a piece of writing about a tool have to be captured by something
// repeatable. A screenshot taken by hand is out of date the moment a panel is
// renamed and nobody finds out until a reader does. This interface is the seam:
// `scripting` depends on it, `ui` implements it and registers itself, and the
// direction of the dependency is unchanged.
//
// Every method here is called from the script worker thread and must not return
// until the UI thread has actually done the work. A screenshot that raced the
// frame it was meant to capture is worse than no screenshot, because it looks
// like a screenshot.
class UiCommands {
public:
    virtual ~UiCommands() = default;

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
};

// Null when nothing is driving a window -- the tests, and anything else that
// links the engines without the frontend. Scripts calling these functions then
// fail with a sentence saying so rather than dereferencing it.
[[nodiscard]] UiCommands* uiCommands();
void setUiCommands(UiCommands* commands);

} // namespace ire::services
