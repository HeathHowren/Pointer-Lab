// "Find out what accesses this address" -- the panel on top of a hardware data
// breakpoint that turns a flood of individual hits into the short list of
// instructions actually responsible.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "engine_disasm/Disassembler.h"

namespace ire::ui {

void UiApp::beginAccessWatch(std::uintptr_t address, domain::ValueType type, bool writesOnly) {
    // A data breakpoint can watch 1, 2, 4 or 8 bytes, so a wider type is
    // watched at 8 and anything odd falls back to a single byte. Watching the
    // first byte of a value still catches every write to it in practice,
    // because nothing writes a field's tail without writing its head.
    auto width = static_cast<std::uint8_t>(domain::valueTypeSize(type));
    if (!domain::isValidWatchLength(width)) {
        width = width >= 8 ? std::uint8_t{8} : std::uint8_t{1};
    }
    // An unaligned address cannot carry a wide watch at all; narrow it rather
    // than refusing outright.
    while (width > 1 && (address % width) != 0) {
        width = static_cast<std::uint8_t>(width / 2);
    }

    if (auto started = services_.startAccessWatch(address, width, writesOnly); !started) {
        notifyError(started.error());
        return;
    }

    accessWatchDetail_ = 0;
    showAccessWatch_ = true;
    ImGui::SetWindowFocus("Access Watch");
    notifyInfo(std::string(writesOnly ? "Watching writes to " : "Watching accesses of ") + domain::toHex(address) +
               ". Make the value change in the target, then look at the list.");
}

void UiApp::renderAccessWatchPanel() {
    ImGui::Begin("Access Watch", &showAccessWatch_);

    auto& watch = services_.accessWatch();
    const bool active = watch.active();

    statusPill(active ? "WATCHING" : "STOPPED",
               active ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));

    if (!active && watch.totalHits() == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("No watch running.");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Right-click an entry in the Address List or a row in the Scan Results and choose "
            "\"Find out what writes to this address\" or \"Find out what accesses this address\".");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "A write watch answers \"what changes this value?\" -- the instruction to patch when you want it to "
            "stop changing. An access watch also catches every read, which is how you find the code that uses a "
            "value rather than the code that sets it.");
        ImGui::End();
        return;
    }

    const auto watched = watch.watchedAddress();
    ImGui::SameLine();
    ImGui::Text("%s at %s", domain::breakpointKindName(watch.kind()), domain::toHex(watched).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%u byte(s), %llu hit(s))", static_cast<unsigned>(watch.watchedLength()),
                        static_cast<unsigned long long>(watch.totalHits()));

    if (active) {
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            services_.stopAccessWatch();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        watch.clear();
        accessWatchDetail_ = 0;
    }

    if (watch.truncated()) {
        ImGui::TextColored(colorFromBytes(214, 154, 70), "%s",
                           ("More than " + std::to_string(engine_debug::AccessWatch::maxSites) +
                            " distinct instructions touched this address; later ones were not recorded.")
                               .c_str());
    }

    const auto sites = watch.sites();
    if (sites.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Nothing has touched this address yet. Do something in the target that would change "
                           "the value -- take damage, fire a shot, spend money.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // The site whose register dump is expanded gets rendered after the table
    // so the interpretation column has the full panel width rather than being
    // clipped inside the narrow Disassembly cell.
    const engine_debug::AccessSite* expanded = nullptr;
    if (ImGui::BeginTable("access-sites", 4, denseTableFlags)) {
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, scaled(60.0f));
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthFixed, scaled(140.0f));
        ImGui::TableSetupColumn("Disassembly", ImGuiTableColumnFlags_WidthStretch);
        // 300 rather than 210: the row carries four SmallButtons and the last
        // two ("NOP", "Script") used to be culled by ItemAdd when the cell
        // clipped, so they were invisible and unclickable.
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, scaled(300.0f));
        ImGui::TableHeadersRow();

        for (const auto& site : sites) {
            ImGui::TableNextRow();
            // A 64-bit trap address truncated to int made two sites that differ
            // only above bit 31 -- a module in high memory and an allocation in
            // low memory happen to be a common pair -- share widget state, so
            // opening the script menu on one popped it up on the other.
            ImGui::PushID(reinterpret_cast<const void*>(site.trapAddress));

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(site.hitCount));

            ImGui::TableNextColumn();
            ImGui::PushFont(monoFont_, monoFont_->LegacySize);
            ImGui::TextUnformatted(domain::toHex(site.address).c_str());
            ImGui::PopFont();
            if (!site.instructionResolved && ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "This is where the CPU reported the trap, not the instruction that did the access.\n"
                    "A data breakpoint fires after the access completes, so the responsible instruction\n"
                    "is the one ending here -- and it could not be identified unambiguously.");
            }

            ImGui::TableNextColumn();
            ImGui::PushFont(monoFont_, monoFont_->LegacySize);
            if (site.instructionResolved) {
                ImGui::TextUnformatted(site.text.c_str());
            } else {
                ImGui::TextDisabled("%s", site.text.c_str());
            }
            ImGui::PopFont();

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Registers")) {
                accessWatchDetail_ = accessWatchDetail_ == site.trapAddress ? 0 : site.trapAddress;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Show")) {
                std::snprintf(disasmAddress_.data(), disasmAddress_.size(), "%s",
                              domain::toHex(site.address).c_str());
                showDisassembly_ = true;
                ImGui::SetWindowFocus("Disassembly");
            }
            ImGui::SameLine();
            // Disabled rather than hidden when the instruction is unknown:
            // NOPing the trap address would destroy an innocent instruction.
            ImGui::BeginDisabled(!site.instructionResolved);
            if (ImGui::SmallButton("NOP")) {
                confirmNopInstruction(site.address, site.bytes.size(), site.text);
            }
            ImGui::SameLine();
            // Nopping an instruction stops it doing everything, including
            // whatever else it was for. A script is the answer when you want it
            // to keep working and only change what it writes, and starting from
            // here means the address, the bytes and the module are already
            // filled in.
            if (ImGui::SmallButton("Script")) {
                ImGui::OpenPopup("##script-template");
            }
            if (ImGui::BeginPopup("##script-template")) {
                ImGui::TextDisabled("Write a script for %s", domain::toHex(site.address).c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Full injection (keeps the original instruction)")) {
                    newScriptFromAddress(site.address, site.bytes, 2);
                }
                if (ImGui::MenuItem("AOB injection (finds the address by its bytes)")) {
                    newScriptFromAddress(site.address, site.bytes, 0);
                }
                if (ImGui::MenuItem("Code cave (fixed address)")) {
                    newScriptFromAddress(site.address, site.bytes, 1);
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();

            if (accessWatchDetail_ == site.trapAddress) {
                expanded = &site;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (expanded != nullptr) {
        // Rendered outside the table so the register interpretation gets the
        // full panel width; inside, it lived in the "Disassembly" cell and
        // the "points 0xF8 into a structure base" text -- the reason this
        // panel exists -- was clipped exactly where it became useful.
        renderAccessWatchDetail(*expanded);
    }

    ImGui::End();
}

void UiApp::confirmNopInstruction(std::uintptr_t address, std::size_t length, const std::string& text) {
    if (length == 0) {
        notifyError("The length of that instruction is not known, so it cannot be replaced safely.");
        return;
    }
    if (!services_.session().attached()) {
        notifyError("Attach to a process before patching it.");
        return;
    }

    // Exactly as many nops as the instruction had bytes. Fewer would leave the
    // tail of the original decoding as a new instruction; more would eat into
    // the next one. This is the same requirement padToInstructionBoundary
    // exists to enforce, met here by construction because the length came from
    // the decoder.
    const std::vector<std::uint8_t> nops(length, 0x90);

    confirmAction("Replace with code that does nothing",
                  "This overwrites\n\n    " + text + "\n\nat " + domain::toHex(address) + " with " +
                      std::to_string(length) +
                      " nop byte(s).\n\nThe instruction stops running entirely. If it was doing something else "
                      "as well as touching this value -- and one instruction often does -- that stops too.\n\n"
                      "The original bytes are recorded, so you can switch this back off in the Patches panel.",
                  "Replace with nops", [this, address, nops, text]() {
                      auto applied = services_.patches().apply(address, nops, "nop " + text, text);
                      if (!applied) {
                          notifyError(applied.error());
                          return;
                      }
                      showPatches_ = true;
                      notifyInfo("Replaced the instruction at " + domain::toHex(address) + " with " +
                                 std::to_string(nops.size()) + " nop(s). Untick it in Patches to put it back.");
                  });
}

void UiApp::renderAccessWatchDetail(const engine_debug::AccessSite& site) {
    ImGui::Separator();
    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    ImGui::TextDisabled("thread %u, trap reported at %s", site.lastContext.threadId,
                        domain::toHex(site.trapAddress).c_str());

    // The reason this panel exists. Reading "EDI+0xF8" off a register here is
    // how a reader gets from "I found my health" to "I found the player
    // structure", which is the whole of Ch09's and Ch12's method.
    if (explainCacheSite_ != site.trapAddress || explainCacheHits_ != site.hitCount) {
        explainCacheSite_ = site.trapAddress;
        explainCacheHits_ = site.hitCount;
        explainCache_ = services_.accessWatch().explain(site.lastContext);
    }
    for (const auto& meaning : explainCache_) {
        const bool structBase = meaning.interpretation.find("structure base") != std::string::npos;
        if (structBase) {
            ImGui::TextColored(colorFromBytes(120, 200, 140), "%-3s %016llX  %s", meaning.name.c_str(),
                               static_cast<unsigned long long>(meaning.value), meaning.interpretation.c_str());
        } else if (meaning.interpretation.empty()) {
            ImGui::TextDisabled("%-3s %016llX", meaning.name.c_str(),
                                static_cast<unsigned long long>(meaning.value));
        } else {
            ImGui::Text("%-3s %016llX  %s", meaning.name.c_str(), static_cast<unsigned long long>(meaning.value),
                        meaning.interpretation.c_str());
        }
    }
    ImGui::PopFont();
}

} // namespace ire::ui
