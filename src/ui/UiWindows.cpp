// The two windows that are documentation rather than tooling.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include <Version.h>

namespace ire::ui {

void UiApp::renderAboutWindow() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(480.0f), 0.0f), ImVec2(scaled(720.0f), FLT_MAX));
    if (ImGui::Begin("About Pointer Lab", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::TextUnformatted(POINTERLAB_PRODUCT_NAME " " POINTERLAB_VERSION_STRING);
        ImGui::TextDisabled("Windows x64 user-mode memory research tool");
        ImGui::Separator();
        ImGui::PushTextWrapPos(680.0f);
        ImGui::TextUnformatted(
            "Pointer Lab is free software licensed under the GNU General Public License, "
            "version 2. It is GPL licensed because it statically links Keystone.");
        ImGui::Dummy(ImVec2(0.0f, scaled(6.0f)));
        ImGui::TextUnformatted("Source code:");
        ImGui::TextDisabled(POINTERLAB_REPO_URL);
        ImGui::Dummy(ImVec2(0.0f, scaled(6.0f)));
        ImGui::TextUnformatted("Third-party components:");
        ImGui::BulletText("Dear ImGui - MIT");
        ImGui::BulletText("Lua 5.4 - MIT");
        ImGui::BulletText("Zydis - MIT");
        ImGui::BulletText("Keystone - GPLv2");
        ImGui::BulletText("Roboto and Cousine fonts - Apache 2.0");
        ImGui::Dummy(ImVec2(0.0f, scaled(6.0f)));
        ImGui::TextDisabled("See LICENSE and THIRD_PARTY_NOTICES.md for full terms.");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void UiApp::renderHelpWindow() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(520.0f), 0.0f), ImVec2(scaled(820.0f), FLT_MAX));
    if (ImGui::Begin("Help", &showHelp_, ImGuiWindowFlags_NoDocking)) {
        // Wrap at the content edge rather than a fixed 780: the window is
        // resizable down to 520, and an absolute wrap position clipped every
        // paragraph on the right instead of re-flowing it.
        ImGui::PushTextWrapPos(0.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextUnformatted("Responsible use");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted(
            "Use Pointer Lab only on software you own or are authorised to analyse. Attaching to "
            "online games or other people's systems may breach their terms of service or the law "
            "where you live. Anti-cheat software commonly treats tools like this as an attack.");

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
        ImGui::SeparatorText("Access");
        ImGui::TextUnformatted(
            "Pointer Lab requests SeDebugPrivilege at startup. Without it, many processes can only "
            "be opened read-only: scanning still works, but writing, freezing, patching, breakpoints "
            "and injection all fail. Run as administrator for full access. The command bar shows "
            "READ-ONLY when access is limited.");

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
        ImGui::SeparatorText("Address input");
        ImGui::TextUnformatted(
            "Every address field is read as hexadecimal, with or without an 0x prefix. "
            "'140001000' and '0x140001000' are the same address.");

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
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

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
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

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
        ImGui::SeparatorText("Pointer chains");
        ImGui::TextUnformatted(
            "A chain added from the pointer scanner is stored as a module name plus an offset and a "
            "list of steps, never as a fixed address. Pointer Lab re-resolves it about twice a second, "
            "so the entry keeps tracking the value after the target restarts somewhere else. An entry "
            "whose chain stops resolving is shown as unresolved rather than reading a stale address.");

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
        ImGui::SeparatorText("Lua");
        ImGui::TextUnformatted(
            "Scripts run on a background thread and can be stopped with the Stop button, so a runaway "
            "loop no longer freezes the application. The standard library is trimmed: io, package, "
            "require, dofile, loadfile and the destructive half of os are removed, because a script "
            "pasted from the internet has no business touching your file system.");

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
        ImGui::SeparatorText("Keyboard");
        ImGui::BulletText("F1 - F12   Toggle freeze on the address list entry with that hotkey.");
        ImGui::TextDisabled(
            "Hotkeys are registered with Windows, so they fire while the target window is in the "
            "foreground - which is when you actually want them. Only keys assigned to an entry are "
            "registered, so Pointer Lab does not take F1-F12 away from everything else on the machine. "
            "A key another application already owns cannot be registered, and falls back to working "
            "only while Pointer Lab has focus.");

        ImGui::Dummy(ImVec2(0.0f, scaled(8.0f)));
        ImGui::SeparatorText("Files");
        ImGui::BulletText("Projects are saved as .iretable files (File menu).");
        ImGui::BulletText("Logs, layout, session, settings and crash dumps live in %%LOCALAPPDATA%%\\PointerLab.");
        ImGui::BulletText("Scan options and which panels are open are remembered between runs.");

        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

} // namespace ire::ui
