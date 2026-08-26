// Looking at and changing memory directly: the hex viewer, the disassembly
// and the breakpoints set from it.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

void UiApp::gotoMemory(std::uintptr_t address) {
    memoryCursor_ = address;
    memoryEditOffset_ = -1;
    copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(address));
    showMemoryViewer_ = true;
}

void UiApp::renderMemoryPanel() {
    ImGui::Begin("Memory Viewer", &showMemoryViewer_);

    constexpr std::size_t bytesPerRow = 16;

    ImGui::SetNextItemWidth(230.0f);
    const bool submitted =
        ImGui::InputTextWithHint("##memory-address", "0x7FF... or client.dll+0x4A2C10", memoryAddress_.data(),
                                 memoryAddress_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go") || submitted) {
        if (auto resolved = resolveAddressOrExplain(memoryAddress_.data())) {
            memoryCursor_ = resolved.value();
            memoryEditOffset_ = -1;
        } else {
            notifyError(resolved.error());
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Bytes", &memoryReadSize_);
    memoryReadSize_ = std::clamp(memoryReadSize_, 16, 4096);

    // Navigation, because the lesson this panel exists for is "walk backwards
    // from a known address until you find the start of the structure". A fixed
    // window you can only jump to by typing a new address made that a
    // hex-arithmetic exercise instead of a scrolling one.
    const auto window = static_cast<std::uintptr_t>(memoryReadSize_);
    const auto move = [this](std::intptr_t delta) {
        // Clamped rather than wrapped: scrolling up from the first page should
        // stop at zero, not reappear at the top of the address space.
        if (delta < 0 && memoryCursor_ < static_cast<std::uintptr_t>(-delta)) {
            memoryCursor_ = 0;
        } else {
            memoryCursor_ = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(memoryCursor_) + delta);
        }
        memoryEditOffset_ = -1;
        copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(memoryCursor_));
    };
    ImGui::SameLine();
    if (ImGui::Button("<< page")) {
        move(-static_cast<std::intptr_t>(window));
    }
    ImGui::SameLine();
    if (ImGui::Button("< row")) {
        move(-static_cast<std::intptr_t>(bytesPerRow));
    }
    ImGui::SameLine();
    if (ImGui::Button("row >")) {
        move(static_cast<std::intptr_t>(bytesPerRow));
    }
    ImGui::SameLine();
    if (ImGui::Button("page >>")) {
        move(static_cast<std::intptr_t>(window));
    }

    std::vector<std::uint8_t> bytes;
    if (services_.session().attached() && memoryCursor_ != 0) {
        if (auto read = services_.session().readBytes(memoryCursor_, static_cast<std::size_t>(memoryReadSize_))) {
            bytes = std::move(read.value());
        }
    }

    // Which bytes changed since the last frame at this same base. Watching a
    // structure repaint itself is how you find the field you are after without
    // scanning for it at all.
    std::vector<bool> changed(bytes.size(), false);
    if (memoryPreviousBase_ == memoryCursor_) {
        for (std::size_t i = 0; i < bytes.size() && i < memoryPrevious_.size(); ++i) {
            changed[i] = bytes[i] != memoryPrevious_[i];
        }
    }
    memoryPreviousBase_ = memoryCursor_;
    memoryPrevious_ = bytes;

    // Where the view actually is, named rather than numbered when it can be.
    // After following three pointers by hand the typed address is long out of
    // date, and "client.dll+0x4A2C10" is the answer worth writing down.
    if (services_.session().attached()) {
        const auto described = services_.symbols().describe(services_.session(), memoryCursor_);
        if (!described.empty()) {
            ImGui::TextColored(staticAddressColor(), "%s", described.c_str());
        } else {
            ImGui::TextDisabled("%s", domain::toHex(memoryCursor_).c_str());
        }
    }

    if (ImGui::BeginTabBar("memory-tabs")) {
        if (ImGui::BeginTabItem("Hex")) {
            if (!bytes.empty()) {
                ImGui::BeginChild("hex-view", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                // The wheel over the hex area moves by rows, which is what
                // every other hex editor does and therefore what fingers expect.
                if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
                    const auto rows = static_cast<std::intptr_t>(-ImGui::GetIO().MouseWheel * 3.0f);
                    move(rows * static_cast<std::intptr_t>(bytesPerRow));
                }
                ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                const float cellWidth = ImGui::CalcTextSize("00 ").x;

                for (std::size_t row = 0; row < bytes.size(); row += bytesPerRow) {
                    const auto rowAddress = memoryCursor_ + row;
                    ImGui::TextDisabled("%s ", domain::toHex(rowAddress).c_str());

                    for (std::size_t col = 0; col < bytesPerRow && row + col < bytes.size(); ++col) {
                        const auto offset = row + col;
                        ImGui::SameLine();
                        ImGui::PushID(static_cast<int>(offset));

                        if (memoryEditOffset_ == static_cast<int>(offset)) {
                            ImGui::SetNextItemWidth(cellWidth * 1.6f);
                            ImGui::SetKeyboardFocusHere();
                            const bool done = ImGui::InputText(
                                "##edit", memoryEditText_.data(), memoryEditText_.size(),
                                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                    ImGuiInputTextFlags_AutoSelectAll);
                            if (done) {
                                const auto value = domain::parseHexBytes(memoryEditText_.data());
                                if (value.empty()) {
                                    notifyError("Enter one byte as two hexadecimal digits.");
                                } else if (auto written = services_.session().writeBytes(
                                               memoryCursor_ + offset, {value.back()});
                                           !written) {
                                    notifyError("Could not write to " +
                                                domain::toHex(memoryCursor_ + offset) + ": " + written.error());
                                }
                                memoryEditOffset_ = -1;
                            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                memoryEditOffset_ = -1;
                            }
                        } else {
                            const bool isChanged = offset < changed.size() && changed[offset];
                            if (isChanged) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
                            }
                            char label[8]{};
                            std::snprintf(label, sizeof(label), "%02X", bytes[offset]);
                            if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None,
                                                  ImVec2(cellWidth, 0.0f))) {
                                memoryEditOffset_ = static_cast<int>(offset);
                                std::snprintf(memoryEditText_.data(), memoryEditText_.size(), "%02X",
                                              bytes[offset]);
                            }
                            if (isChanged) {
                                ImGui::PopStyleColor();
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s\n\nClick to edit this byte.\nRight-click to follow it as a "
                                                  "pointer.",
                                                  domain::toHex(memoryCursor_ + offset).c_str());
                            }
                            if (ImGui::BeginPopupContextItem("##cell-menu")) {
                                if (ImGui::MenuItem("Follow as pointer")) {
                                    // The move that makes a chain walkable by
                                    // hand: read the pointer here and land on
                                    // whatever it names.
                                    if (auto pointer = services_.session().readPointer(memoryCursor_ + offset)) {
                                        gotoMemory(pointer.value());
                                    } else {
                                        notifyError("Nothing readable at " +
                                                    domain::toHex(memoryCursor_ + offset) + ".");
                                    }
                                }
                                if (ImGui::MenuItem("Disassemble here")) {
                                    copyText(disasmAddress_.data(), disasmAddress_.size(),
                                             domain::toHex(memoryCursor_ + offset));
                                    showDisassembly_ = true;
                                    ImGui::SetWindowFocus("Disassembly");
                                }
                                if (ImGui::MenuItem("Find out what writes here")) {
                                    beginAccessWatch(memoryCursor_ + offset, domain::ValueType::Int32, true);
                                }
                                if (ImGui::MenuItem("Dissect from here")) {
                                    // Scrolling back to where an object starts
                                    // is the hard half of reading a structure;
                                    // this is the payoff for having done it.
                                    dissect(memoryCursor_ + offset);
                                }
                                ImGui::EndPopup();
                            }
                        }
                        ImGui::PopID();
                    }

                    ImGui::SameLine(0, 24);
                    std::string ascii;
                    for (std::size_t col = 0; col < bytesPerRow && row + col < bytes.size(); ++col) {
                        const unsigned char c = bytes[row + col];
                        ascii.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
                    }
                    ImGui::TextUnformatted(ascii.c_str());
                }
                ImGui::PopFont();
                ImGui::EndChild();
            } else if (!services_.session().attached()) {
                ImGui::TextDisabled("Attach to a target to read memory.");
            } else {
                ImGui::TextDisabled("Nothing readable at %s.", domain::toHex(memoryCursor_).c_str());
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
            ImGui::TextDisabled("Patch raw bytes at %s.", domain::toHex(memoryCursor_).c_str());
            ImGui::InputTextWithHint("Bytes", "90 90 CC", memoryPatch_.data(), memoryPatch_.size());
            if (ImGui::Button("Patch at address")) {
                auto patch = domain::parseHexBytes(memoryPatch_.data());
                if (memoryCursor_ == 0) {
                    notifyError("Enter an address above and press Go first.");
                } else if (patch.empty()) {
                    notifyError("Enter the replacement bytes as hexadecimal, for example 90 90 CC.");
                } else {
                    const auto target = memoryCursor_;
                    confirmAction(
                        "Overwrite memory in the target?",
                        "This writes " + std::to_string(patch.size()) + " byte(s) over " +
                            domain::toHex(target) +
                            ". The bytes being replaced are recorded first, so this can be switched back off "
                            "in the Patches panel.",
                        "Patch",
                        [this, target, patch] {
                            auto applied = services_.patches().apply(target, patch, "memory patch");
                            if (!applied) {
                                notifyError("Patch failed: " + applied.error());
                            } else {
                                showPatches_ = true;
                                notifyInfo("Wrote " + std::to_string(patch.size()) + " bytes at " +
                                           domain::toHex(target) + ".");
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
void UiApp::renderDisassemblyPanel() {
    ImGui::Begin("Disassembly", &showDisassembly_);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Address", "0x7FF... or client.dll+0x4A2C10", disasmAddress_.data(), disasmAddress_.size());
    const auto address = resolveAddress(disasmAddress_.data());
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
                    auto assembled = services_.assembler().assemble(assemblerText_.data(), *address,
                                                                   services_.session().bitness());
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
                        message += "The bytes being replaced are recorded first, so this can be switched back "
                                   "off in the Patches panel.";

                        // Describe what is being overwritten while it is still
                        // there -- once patched, the listing shows the patch.
                        std::string replaced;
                        for (const auto& instruction :
                             services_.disassembler().disassemble(services_.session(), target, 4)) {
                            if (instruction.address >= target + bytes.size()) {
                                break;
                            }
                            if (!replaced.empty()) {
                                replaced += "; ";
                            }
                            replaced += instruction.text;
                        }

                        confirmAction("Overwrite code in the target?", std::move(message), "Patch",
                            [this, target, bytes, replaced] {
                                auto applied = services_.patches().apply(
                                    target, bytes, "assembler patch", replaced);
                                if (!applied) {
                                    notifyError("Patch failed: " + applied.error());
                                } else {
                                    showPatches_ = true;
                                    notifyInfo("Patched " + std::to_string(bytes.size()) + " bytes at " +
                                               domain::toHex(target) + ".");
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
    ImGui::InputTextWithHint("Address", "0x7FF... or client.dll+0x4A2C10", breakpointAddress_.data(), breakpointAddress_.size());
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
        if (auto address = resolveAddress(breakpointAddress_.data())) {
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
                    // Named and counted from the captured context's own bitness.
                    // A 32-bit target has no r8-r15, and printing its EAX as a
                    // 16-digit "RAX" invites the reader to hunt for meaning in
                    // eight leading zeroes that are an artefact of our storage.
                    const auto count = domain::registerCount(r.bitness);
                    const char* format = r.bitness == domain::Bitness::X86 ? "%-3s  %08llX" : "%-3s  %016llX";
                    for (std::size_t i = 0; i < count; ++i) {
                        ImGui::Text(format, domain::registerName(r.bitness, i),
                                    static_cast<unsigned long long>(domain::registerValue(r, i)));
                        if (i % 2 == 0 && i + 1 < count) {
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
