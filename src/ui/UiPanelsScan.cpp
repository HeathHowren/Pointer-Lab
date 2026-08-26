// Finding values and keeping them: the scanner and the address list it feeds.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "engine_symbols/SymbolTable.h"

namespace ire::ui {

namespace {

// "10, 8, 0x1C" or "+10 +8 +1C" or "10 -4". Hexadecimal throughout, like every
// other number in this tool; an offset that read as decimal because it happened
// to contain no letters would be a silent wrong answer.
std::optional<std::vector<std::ptrdiff_t>> parseOffsets(const std::string& text) {
    std::vector<std::ptrdiff_t> offsets;
    std::string token;
    const auto flush = [&]() -> bool {
        if (token.empty()) {
            return true;
        }
        bool negative = false;
        std::size_t start = 0;
        if (token[0] == '+' || token[0] == '-') {
            negative = token[0] == '-';
            start = 1;
        }
        const auto value = domain::parseAddress(token.substr(start));
        if (!value) {
            return false;
        }
        const auto magnitude = static_cast<std::ptrdiff_t>(*value);
        offsets.push_back(negative ? -magnitude : magnitude);
        token.clear();
        return true;
    };

    for (const char c : text) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!flush()) {
                return std::nullopt;
            }
            continue;
        }
        // A sign starts a new term unless it is the first character of one.
        if ((c == '+' || c == '-') && !token.empty()) {
            if (!flush()) {
                return std::nullopt;
            }
        }
        token.push_back(c);
    }
    if (!flush()) {
        return std::nullopt;
    }
    return offsets;
}

// Roots the chain in a module whenever the base lands inside one, which is what
// makes it survive a restart. A base that is not in any module is recorded
// absolute, and the UI says so rather than pretending otherwise.
domain::PointerChain chainFrom(domain::TargetSession& session, std::uintptr_t base,
                               std::vector<std::ptrdiff_t> offsets) {
    domain::PointerChain chain;
    chain.offsets = std::move(offsets);
    if (const auto module = engine_symbols::SymbolTable::moduleAt(session, base)) {
        chain.moduleName = module->name;
        chain.moduleBase = module->base;
        chain.moduleOffset = base - module->base;
    } else {
        chain.moduleOffset = base;
    }
    return chain;
}

std::string describeChain(const domain::PointerChain& chain) {
    std::string text = chain.moduleRooted()
                           ? domain::narrow(chain.moduleName) + "+" + domain::toHex(chain.moduleOffset)
                           : domain::toHex(chain.moduleOffset);
    for (const auto offset : chain.offsets) {
        text += offset < 0 ? " -> -" + domain::toHex(static_cast<std::uintptr_t>(-offset))
                           : " -> +" + domain::toHex(static_cast<std::uintptr_t>(offset));
    }
    return text;
}

} // namespace

void UiApp::renderScanPanel() {
    ImGui::Begin("Scanner");
    const auto typeNames = valueTypeNames();
    const auto modeNames = scanModeNames();

    const auto type = valueTypeFromIndex(scanTypeIndex_);
    const auto mode = scanModeFromIndex(scanModeIndex_);
    const bool wantsSecond = domain::modeUsesSecondValue(mode);
    // Changed/Unchanged/Increased/Decreased/Same as first compare against an
    // earlier scan and need no typed value. Requiring one meant the flagship
    // unknown-then-narrow workflow silently did nothing.
    const bool wantsValue = domain::modeUsesValue(mode);
    const bool supported = engine_scan::modeSupportsType(mode, type);

    ImGui::BeginChild("scan-controls", ImVec2(0, supported ? 132.0f : 152.0f), true);
    ImGui::TextDisabled("Scan setup");
    ImGui::SameLine();
    helpMarker("Use Unknown initial for broad baselines, then Changed/Unchanged/Increased/Decreased to narrow.\n\n"
               "When you know how much a value moved, Increased by and Decreased by usually finish the search in "
               "one step: \"I lost exactly 7 health\" is far more selective than \"it went down\".\n\n"
               "Same as first scan compares against the very first scan of the run rather than the previous one, "
               "which is how you find a value that changed and came back.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(118.0f);
    ImGui::Combo("Type", &scanTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::Combo("Mode", &scanModeIndex_, modeNames.data(), static_cast<int>(modeNames.size()));
    ImGui::SameLine();
    ImGui::BeginDisabled(!wantsValue);
    ImGui::SetNextItemWidth(wantsSecond ? 150.0f : -1.0f);
    ImGui::InputTextWithHint("##scan-value", domain::isStringType(type) ? "text to find" : "value or byte pattern",
                             scanText_.data(), scanText_.size());
    if (wantsSecond) {
        ImGui::SameLine();
        ImGui::TextUnformatted("and");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##scan-value2", "upper bound", scanText2_.data(), scanText2_.size());
    }
    ImGui::EndDisabled();

    if (!supported) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextWrapped("%s cannot be %s: ordering has no meaning for %s.", domain::scanModeName(mode),
                           "applied to this type",
                           type == domain::ValueType::Bytes ? "a byte pattern" : "text");
        ImGui::PopStyleColor();
    }

    if (domain::isStringType(type)) {
        ImGui::Checkbox("Ignore case", &scanCaseInsensitive_);
        ImGui::SameLine();
        helpMarker("Folds A-Z only. Matching the rest correctly needs the Unicode case tables and a locale, and a "
                   "scanner that folded some characters but not others without saying which would be worse than "
                   "one that is clear about where it stops.\n\n"
                   "wstr is what Windows calls Unicode: two bytes per character. Most player names and chat text "
                   "in a Windows game are stored that way, so try wstr when str finds nothing.");
    }

    if (showScannerFilters_ || ImGui::TreeNodeEx("Region filters", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Writable only", &scanWritableOnly_);
        ImGui::SameLine();
        ImGui::Checkbox("Executable only", &scanExecutableOnly_);
        if (!showScannerFilters_) {
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    std::optional<domain::ScanValue> scanValue;
    if (wantsValue) {
        scanValue = domain::parseScanValue(type, scanText_.data());
        if (scanValue) {
            scanValue->caseInsensitive = scanCaseInsensitive_;
            if (wantsSecond) {
                // Parsed with the same type as the lower bound, so "between 10
                // and 20" means the same thing in both halves.
                if (auto upper = domain::parseScanValue(type, scanText2_.data())) {
                    scanValue->bytes2 = std::move(upper->bytes);
                    scanValue->text2 = scanText2_.data();
                } else {
                    scanValue.reset();
                }
            }
        }
    } else {
        // The mode compares against an earlier scan, so there is no needle --
        // only a width to read back. The variable-length types have no width of
        // their own and take the previous scan's instead.
        domain::ScanValue placeholder;
        placeholder.type = type;
        placeholder.bytes.assign(domain::valueTypeSize(type), 0);
        scanValue = std::move(placeholder);
    }

    const auto currentOptions = [this] {
        engine_scan::ScanOptions options;
        options.writableOnly = scanWritableOnly_;
        options.executableOnly = scanExecutableOnly_;
        options.maxResults = static_cast<std::size_t>(std::max(1000, scanMaxResults_));
        options.floatEpsilon = static_cast<double>(scanFloatEpsilon_);
        return options;
    };

    // The same complaint from both buttons, so a rejected scan says the same
    // thing wherever it was started from.
    const auto valueComplaint = [&] {
        if (type == domain::ValueType::Bytes) {
            return std::string("Enter a byte pattern such as 48 8B ?? 24.");
        }
        if (domain::isStringType(type)) {
            return std::string("Enter the text to search for.");
        }
        if (wantsSecond) {
            return std::string("Enter both bounds, for example 100 and 200.");
        }
        return std::string("Scan value is not valid for the selected type.");
    };

    if (ImGui::Button("First scan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process before scanning.");
        } else if (!supported) {
            notifyError(std::string(domain::scanModeName(mode)) + " cannot be used with " +
                        domain::valueTypeName(type) + ": ordering has no meaning for it.");
        } else if (!scanValue) {
            notifyError(valueComplaint());
        } else {
            services_.scanJob().setOptions(currentOptions());
            services_.scanJob().startFirst(mode, *scanValue);
            if (engine_scan::modeNeedsBaseline(mode)) {
                notifyInfo(std::string(domain::scanModeName(mode)) +
                           " needs something to compare against, so this first scan records a baseline. "
                           "Let the value change, then press Next scan.");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next scan")) {
        auto previous = services_.scanJob().results();
        if (previous.empty()) {
            notifyError("Run a first scan before filtering with Next scan.");
        } else if (!supported) {
            notifyError(std::string(domain::scanModeName(mode)) + " cannot be used with " +
                        domain::valueTypeName(type) + ": ordering has no meaning for it.");
        } else if (!scanValue) {
            notifyError(valueComplaint());
        } else {
            services_.scanJob().setOptions(currentOptions());
            services_.scanJob().startNext(mode, *scanValue, std::move(previous));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services_.scanJob().cancel();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputInt("Result limit", &scanMaxResults_, 0, 0);
    scanMaxResults_ = std::clamp(scanMaxResults_, 1000, 20000000);
    ImGui::SameLine();
    helpMarker("Scanning stops once this many results are found. Unknown-initial scans of a large "
               "process can exceed it easily; narrow with region filters or raise the limit.");
    if (type == domain::ValueType::Float || type == domain::ValueType::Double) {
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputFloat("Float tolerance", &scanFloatEpsilon_, 0.0f, 0.0f, "%.5f");
        scanFloatEpsilon_ = std::clamp(scanFloatEpsilon_, 0.0f, 1000.0f);
        ImGui::SameLine();
        helpMarker("Exact float matches allow this much difference. A displayed 100.0 is rarely "
                   "bit-identical to the stored value, so a tolerance of 0 usually finds nothing.");
    }

    const auto progress = services_.scanJob().progress();
    ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
    if (progress.truncated) {
        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(232, 184, 92));
        ImGui::TextWrapped("Result limit reached, so this is only part of the address space. "
                           "Narrow the scan with region filters or raise the limit.");
        ImGui::PopStyleColor();
    }

    auto results = services_.scanJob().results();
    constexpr std::size_t displayLimit = 10000;
    const std::size_t count = std::min<std::size_t>(results.size(), displayLimit);
    if (results.size() > displayLimit) {
        // Silently showing the first 10,000 of a much larger set read as
        // "these are all the results".
        ImGui::TextDisabled("Showing the first %zu of %zu results. Narrow the scan to see the rest.",
                            count, results.size());
    } else {
        ImGui::TextDisabled("%zu result%s", results.size(), results.size() == 1 ? "" : "s");
    }
    ImGui::SameLine();
    ImGui::TextDisabled(" -- ");
    ImGui::SameLine();
    ImGui::TextColored(staticAddressColor(), "green");
    ImGui::SameLine();
    ImGui::TextDisabled("is a static address: it sits inside a loaded module, so it is at the same "
                        "module+offset every run.");

    // Snapshotted once for the whole table rather than looked up per row. The
    // clipper keeps the row count small, but session.modules() copies a vector
    // under a lock and doing that thirty times a frame is thirty times too many.
    const auto modules = services_.session().modules();
    const auto moduleAt = [&modules](std::uintptr_t address) -> const domain::ModuleInfo* {
        const auto module = std::find_if(modules.begin(), modules.end(), [address](const domain::ModuleInfo& m) {
            return address >= m.base && address < m.base + m.size;
        });
        return module == modules.end() ? nullptr : &*module;
    };

    if (ImGui::BeginTable("scan-results", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Previous");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Only the visible rows are laid out; formatting 10,000 rows every
        // frame was a large amount of pointless work.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto i = static_cast<std::size_t>(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const auto* module = moduleAt(results[i].address);
                if (module != nullptr) {
                    ImGui::TextColored(staticAddressColor(), "%s", domain::toHex(results[i].address).c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s+%s\n\nStatic: inside a loaded module, so this address is the same "
                                          "offset from that module in every run.",
                                          domain::narrow(module->name).c_str(),
                                          domain::toHex(results[i].address - module->base).c_str());
                    }
                } else {
                    ImGui::TextUnformatted(domain::toHex(results[i].address).c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::formatValue(type, results[i].previous).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::formatValue(type, results[i].current).c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                if (ImGui::SmallButton("Add")) {
                    services_.addressList().add(results[i].address, type, "Scan result", "Scan");
                    notifyInfo("Added " + domain::toHex(results[i].address) + " to the address list.");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("View")) {
                    gotoMemory(results[i].address);
                    copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(results[i].address));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Find...")) {
                    ImGui::OpenPopup("##find-access-result");
                }
                if (ImGui::BeginPopup("##find-access-result")) {
                    ImGui::TextDisabled("%s", domain::toHex(results[i].address).c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Find out what writes to this address")) {
                        beginAccessWatch(results[i].address, type, true);
                    }
                    if (ImGui::MenuItem("Find out what accesses this address")) {
                        beginAccessWatch(results[i].address, type, false);
                    }
                    ImGui::Separator();
                    // The other direction: not "what touches this value" but
                    // "what is this value part of".
                    if (ImGui::MenuItem("Dissect this")) {
                        dissect(results[i].address);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
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
        ImGui::BeginChild("address-editor", ImVec2(0, addAsPointer_ ? 178.0f : 126.0f), true);
        ImGui::SetNextItemWidth(210.0f);
        ImGui::InputTextWithHint(addAsPointer_ ? "Base" : "Address", "0x7FF... or client.dll+0x4A2C10",
                                 addAddress_.data(), addAddress_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Type", &addTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
        ImGui::SameLine();
        ImGui::Checkbox("Pointer", &addAsPointer_);
        ImGui::SameLine();
        helpMarker("With Pointer ticked, the box above holds the *base* of a chain rather than the value's own "
                   "address, and the offsets are how to walk from it: read a pointer at the base, add the first "
                   "offset, read a pointer there, add the next, and so on.\n\n"
                   "This is the point of the whole exercise. A value's own address is different every launch; a "
                   "chain rooted in a module is the same every launch, which is why it is worth writing down.\n\n"
                   "Leave the offsets empty to record the base itself -- useful for a static value that needs no "
                   "dereferencing at all.");

        if (addAsPointer_) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("Offsets", "hex, outermost first: 10, 1C, 4", addPointerOffsets_.data(),
                                     addPointerOffsets_.size());

            // Resolved live, because a chain that is one offset wrong resolves
            // to a plausible address and reads a plausible number, and the only
            // way to tell is to look at what it lands on.
            const auto base = resolveAddress(addAddress_.data());
            const auto offsets = parseOffsets(addPointerOffsets_.data());
            if (!base) {
                ImGui::TextDisabled("Enter a base address or a module+offset.");
            } else if (!offsets) {
                ImGui::TextColored(colorFromBytes(232, 184, 92), "Offsets must be hexadecimal, separated by "
                                                                 "commas or spaces.");
            } else {
                const auto chain = chainFrom(services_.session(), *base, *offsets);
                ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                ImGui::TextDisabled("%s", describeChain(chain).c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                auto resolved = engine_pointer::resolveChain(services_.session(), chain);
                if (resolved) {
                    ImGui::Text("= %s", domain::toHex(resolved.value()).c_str());
                    if (auto bytes = services_.session().readBytes(
                            resolved.value(),
                            std::max<std::size_t>(1, domain::valueTypeSize(valueTypeFromIndex(addTypeIndex_))))) {
                        ImGui::SameLine();
                        ImGui::TextDisabled(
                            "(%s)",
                            domain::formatValue(valueTypeFromIndex(addTypeIndex_), bytes.value()).c_str());
                    }
                } else {
                    ImGui::TextColored(colorFromBytes(232, 184, 92), "%s", resolved.error().c_str());
                }
                if (!chain.moduleRooted()) {
                    ImGui::TextColored(colorFromBytes(232, 184, 92),
                                       "The base is not inside any loaded module, so this chain will not survive "
                                       "a restart.");
                }
            }
        }

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
            auto resolved = resolveAddressOrExplain(addAddress_.data());
            if (!resolved) {
                notifyError(resolved.error());
            } else if (addAsPointer_) {
                const auto offsets = parseOffsets(addPointerOffsets_.data());
                if (!offsets) {
                    notifyError("Offsets must be hexadecimal, separated by commas or spaces.");
                } else {
                    const auto type = valueTypeFromIndex(addTypeIndex_);
                    auto chain = chainFrom(services_.session(), resolved.value(), *offsets);
                    // Removing and re-adding rather than editing in place: an
                    // entry gains and loses its chain here, and the address list
                    // resolves chains on its own schedule.
                    if (editEntryId_ != 0) {
                        services_.addressList().remove(editEntryId_);
                        editEntryId_ = 0;
                    }
                    const auto id = services_.addressList().addChain(std::move(chain), type,
                                                                     addDescription_.data(), addGroup_.data());
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
                }
            } else {
                const auto address = std::optional<std::uintptr_t>(resolved.value());
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

    // Same snapshot-once reasoning as the scan results table.
    const auto modules = services_.session().modules();
    const auto moduleAt = [&modules](std::uintptr_t address) -> const domain::ModuleInfo* {
        const auto module = std::find_if(modules.begin(), modules.end(), [address](const domain::ModuleInfo& m) {
            return address >= m.base && address < m.base + m.size;
        });
        return module == modules.end() ? nullptr : &*module;
    };

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
            if (entry.chain && !entry.resolved) {
                // Showing the last known address as though it were live would be
                // a lie: the chain no longer leads anywhere.
                ImGui::TextDisabled("unresolved");
            } else if (const auto* module = moduleAt(entry.address); module != nullptr) {
                ImGui::TextColored(staticAddressColor(), "%s", domain::toHex(entry.address).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s+%s\n\nStatic: inside a loaded module, so this address is the same "
                                      "offset from that module in every run.",
                                      domain::narrow(module->name).c_str(),
                                      domain::toHex(entry.address - module->base).c_str());
                }
            } else {
                ImGui::TextUnformatted(domain::toHex(entry.address).c_str());
            }
            if (entry.chain) {
                ImGui::SameLine();
                // Amber for a chain that cannot survive a restart, so the one
                // property that makes a chain worth having is visible on the
                // row rather than only in the editor that created it.
                if (entry.chain->moduleRooted()) {
                    ImGui::TextDisabled("(P)");
                } else {
                    ImGui::TextColored(colorFromBytes(232, 184, 92), "(P)");
                }
                if (ImGui::IsItemHovered()) {
                    std::string tip = describeChain(*entry.chain);
                    tip += entry.chain->moduleRooted()
                               ? "\n\nTracked as a pointer chain, so it re-resolves when the target restarts."
                               : "\n\nThe base is an absolute address rather than a module offset, so this chain "
                                 "will not survive a restart.";
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::valueTypeName(entry.type));
            ImGui::TableNextColumn();
            std::string current = "<unreadable>";
            if (entry.chain && !entry.resolved) {
                current = "<chain broken>";
            } else if (services_.session().attached()) {
                if (auto bytes = services_.session().readBytes(entry.address, std::max<std::size_t>(1, domain::valueTypeSize(entry.type)))) {
                    current = domain::formatValue(entry.type, bytes.value());
                }
            }
            ImGui::TextUnformatted(current.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(entry.id)));
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
                addAsPointer_ = entry.chain.has_value();
                if (entry.chain) {
                    // Put the base back the way it was written, not as the
                    // absolute address it currently resolves to -- otherwise
                    // pressing Apply would quietly convert a chain that survives
                    // restarts into one that does not.
                    copyText(addAddress_.data(), addAddress_.size(),
                             entry.chain->moduleRooted()
                                 ? domain::narrow(entry.chain->moduleName) + "+" +
                                       domain::toHex(entry.chain->moduleOffset)
                                 : domain::toHex(entry.chain->moduleOffset));
                    std::string offsets;
                    for (const auto offset : entry.chain->offsets) {
                        if (!offsets.empty()) {
                            offsets += ", ";
                        }
                        offsets += offset < 0 ? "-" + domain::toHex(static_cast<std::uintptr_t>(-offset))
                                              : domain::toHex(static_cast<std::uintptr_t>(offset));
                    }
                    copyText(addPointerOffsets_.data(), addPointerOffsets_.size(), offsets);
                } else {
                    copyText(addAddress_.data(), addAddress_.size(), domain::toHex(entry.address));
                    addPointerOffsets_.fill('\0');
                }
                copyText(addDescription_.data(), addDescription_.size(), entry.description);
                copyText(addGroup_.data(), addGroup_.size(), entry.group);
                copyText(addHotkey_.data(), addHotkey_.size(), entry.hotkey);
                copyText(addValue_.data(), addValue_.size(), domain::formatValue(entry.type, entry.frozenValue));
                const auto types = domain::valueTypes();
                const auto it = std::find(types.begin(), types.end(), entry.type);
                addTypeIndex_ = it == types.end() ? 4 : static_cast<int>(std::distance(types.begin(), it));
            }
            ImGui::SameLine();
            // This used to write whatever happened to be typed in the shared
            // manual-editor box at the top of the panel, so pressing Write on
            // one row could store a value meant for a different one.
            if (ImGui::SmallButton("Write")) {
                rowWriteId_ = entry.id;
                copyText(rowWriteValue_.data(), rowWriteValue_.size(), current);
                ImGui::OpenPopup("##write-value");
            }
            if (ImGui::BeginPopup("##write-value")) {
                ImGui::TextDisabled("%s at %s", domain::valueTypeName(entry.type), domain::toHex(entry.address).c_str());
                ImGui::SetNextItemWidth(180.0f);
                const bool submitted = ImGui::InputText("##value", rowWriteValue_.data(), rowWriteValue_.size(),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Write") || submitted) && rowWriteId_ == entry.id) {
                    if (auto value = domain::parseScanValue(entry.type, rowWriteValue_.data())) {
                        if (services_.addressList().updateValue(entry.id, value->bytes)) {
                            notifyInfo("Wrote " + std::string(rowWriteValue_.data()) + " to " + domain::toHex(entry.address) + ".");
                        } else {
                            notifyError("Could not write to " + domain::toHex(entry.address) + ".");
                        }
                    } else {
                        notifyError("Value is not valid for type " + std::string(domain::valueTypeName(entry.type)) + ".");
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            // The question every reader asks next, so it belongs on the row
            // rather than behind a panel they would have to know to open.
            if (ImGui::SmallButton("Find...")) {
                ImGui::OpenPopup("##find-access");
            }
            if (ImGui::BeginPopup("##find-access")) {
                ImGui::TextDisabled("%s", domain::toHex(entry.address).c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Find out what writes to this address")) {
                    beginAccessWatch(entry.address, entry.type, true);
                }
                if (ImGui::MenuItem("Find out what accesses this address")) {
                    beginAccessWatch(entry.address, entry.type, false);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Dissect this")) {
                    dissect(entry.address);
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                const auto id = entry.id;
                const auto label = entry.description.empty() ? domain::toHex(entry.address) : entry.description;
                confirmAction("Remove address entry?",
                              "Remove \"" + label + "\" from the address list? Any freeze on it stops.",
                              "Remove",
                              [this, id] {
                                  if (services_.addressList().remove(id)) {
                                      notifyInfo("Entry removed.");
                                  }
                              });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace ire::ui
