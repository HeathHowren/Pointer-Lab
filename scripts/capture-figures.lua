-- Captures one PNG per panel, plus the default arrangement, into ./figures.
--
--     PointerLab.exe --script scripts\capture-figures.lua
--
-- Run from the repository root; the paths below are relative to the working
-- directory, and the directory is created if it is not there.
--
-- The point of this file is that it is re-run rather than remembered. A figure
-- captured by hand is correct on the day and silently wrong afterwards: a panel
-- gets renamed, a control moves, and nobody finds out until a reader follows an
-- instruction that no longer matches the picture beside it. Re-running this on
-- every release either produces the new pictures or fails on the panel that no
-- longer exists, and either of those is better than not knowing.
--
-- Two things are load-bearing and neither is obvious:
--
--   * The window is given an exact client size, so every figure across every
--     release is the same number of pixels and a reader comparing two of them
--     is comparing the tool rather than somebody's window manager.
--   * Every capture waits several frames. Opening a panel takes effect on the
--     next frame and its docking settles on the one after; a screenshot taken
--     immediately catches the layout mid-move.
--
-- Panels dragged out of the main window become separate OS windows and will not
-- appear, because this captures the window's own back buffer rather than the
-- screen. That is deliberate -- nothing behind the window, and no notification
-- that happened to appear, can end up in a figure -- but it does mean figures
-- must be captured from the docked layout, which is what set_layout("default")
-- is for.

local directory = "figures/"
local width, height = 1600, 1000
local settle = 12

local function capture(name)
    wait_frames(settle)
    local ok, err = screenshot(directory .. name .. ".png")
    if not ok then
        error("could not write " .. name .. ".png: " .. tostring(err))
    end
    print("captured " .. name)
end

local function panel(name, file)
    local ok, err = select_panel(name)
    if not ok then
        -- Loud on purpose. A panel that no longer answers to its name is
        -- exactly the thing this script exists to catch.
        error("no panel called " .. name .. ": " .. tostring(err))
    end
    capture(file)
end

assert(set_window_size(width, height))
assert(set_layout("default"))
capture("layout-default")

panel("Process Selection", "panel-process-selection")
panel("Scanner",           "panel-scanner")
panel("Address List",      "panel-address-list")
panel("Memory Viewer",     "panel-memory-viewer")
panel("Disassembly",       "panel-disassembly")
panel("Breakpoints",       "panel-breakpoints")
panel("Access Watch",      "panel-access-watch")
panel("Patches",           "panel-patches")
panel("Symbols",           "panel-symbols")
panel("Scripts",           "panel-scripts")
panel("Structures",        "panel-structures")
panel("Speed and Export",  "panel-speed-and-export")
panel("Pointer Scanner",   "panel-pointer-scanner")
panel("Lua Scanner",       "panel-lua-scanner")
panel("Injection",         "panel-injection")
panel("Modules",           "panel-modules")
panel("Memory Regions",    "panel-memory-regions")
panel("Logs",              "panel-logs")
panel("Lua Console",       "panel-lua-console")

print("done")
quit()
