// Looking at and changing memory directly: the hex viewer, the disassembly
// and the breakpoints set from it.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

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

} // namespace ire::ui
