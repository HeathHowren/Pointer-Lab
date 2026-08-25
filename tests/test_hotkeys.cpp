// Global hotkey registration.
//
// Hotkeys used to be read from ImGui, which meant they only worked while
// Pointer Lab had focus -- the one time you do not need them, since the whole
// point is to toggle a freeze while the target is in the foreground. These
// register with the OS instead, which introduces a failure mode ImGui did not
// have: another application may already own the key. That refusal has to be
// reported and fall back to in-window handling rather than leaving the user with
// a hotkey that silently does nothing.

#include <catch2/catch_test_macros.hpp>

#include "platform_win32/Win32Platform.h"

#include <Windows.h>

using namespace ire;
using platform_win32::GlobalHotkeys;

namespace {

// A message-only window: enough to own a hotkey registration, with nothing on
// screen and no message loop to run.
class MessageWindow {
public:
    MessageWindow() {
        hwnd_ = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    }
    ~MessageWindow() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
        }
    }

    MessageWindow(const MessageWindow&) = delete;
    MessageWindow& operator=(const MessageWindow&) = delete;

    [[nodiscard]] HWND get() const { return hwnd_; }

private:
    HWND hwnd_{};
};

} // namespace

// Which F-key an address list entry's hotkey string names. Getting this wrong
// means registering the wrong key, or registering nothing at all and leaving the
// user with a hotkey that quietly does not work.
TEST_CASE("A hotkey name maps to the F-key it names, or to nothing", "[hotkey]") {
    CHECK(GlobalHotkeys::idFor("F1") == 1);
    CHECK(GlobalHotkeys::idFor("F9") == 9);
    CHECK(GlobalHotkeys::idFor("F12") == 12);
    // The address list matches hotkeys case-insensitively, so this has to agree.
    CHECK(GlobalHotkeys::idFor("f5") == 5);

    SECTION("anything that is not an F-key in range has no id") {
        CHECK_FALSE(GlobalHotkeys::idFor("").has_value());
        CHECK_FALSE(GlobalHotkeys::idFor("F").has_value());
        CHECK_FALSE(GlobalHotkeys::idFor("F0").has_value());
        // Some keyboards do have an F13. Pointer Lab's hotkeys stop at F12, and
        // silently registering nothing would be worse than saying no.
        CHECK_FALSE(GlobalHotkeys::idFor("F13").has_value());
        CHECK_FALSE(GlobalHotkeys::idFor("Ctrl+F1").has_value());
        CHECK_FALSE(GlobalHotkeys::idFor("F1x").has_value());
        CHECK_FALSE(GlobalHotkeys::idFor("Delete").has_value());
    }
}

TEST_CASE("Only the requested keys are claimed, and released again", "[hotkey]") {
    MessageWindow window;
    REQUIRE(window.get() != nullptr);

    GlobalHotkeys hotkeys;
    const auto refused = hotkeys.apply(window.get(), {9, 10});
    if (!refused.empty()) {
        // Nothing to prove on a desktop where these are already spoken for.
        SKIP("F9 or F10 is already registered by another application on this machine.");
    }

    CHECK(hotkeys.owns(9));
    CHECK(hotkeys.owns(10));
    // The keys nobody asked for stay available to the rest of the machine.
    CHECK_FALSE(hotkeys.owns(1));
    CHECK_FALSE(hotkeys.owns(12));

    SECTION("a key another owner already holds is refused rather than stolen") {
        GlobalHotkeys second;
        MessageWindow other;
        REQUIRE(other.get() != nullptr);

        const auto denied = second.apply(other.get(), {9, 11});
        REQUIRE(denied.size() == 1);
        CHECK(denied.front() == 9);
        // The one that was free still registered, so a single refusal does not
        // cost the user the rest of their hotkeys.
        CHECK(second.owns(11));
        CHECK_FALSE(second.owns(9));
    }

    SECTION("applying a new set releases the old one") {
        const auto again = hotkeys.apply(window.get(), {11});
        CHECK(again.empty());
        CHECK(hotkeys.owns(11));
        CHECK_FALSE(hotkeys.owns(9));
        CHECK_FALSE(hotkeys.owns(10));

        // Genuinely released, not merely forgotten: a fresh owner can take them.
        GlobalHotkeys second;
        MessageWindow other;
        REQUIRE(other.get() != nullptr);
        CHECK(second.apply(other.get(), {9, 10}).empty());
    }

    hotkeys.unregisterAll();
    CHECK_FALSE(hotkeys.owns(9));
}
