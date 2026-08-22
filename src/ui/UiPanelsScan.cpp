// Finding values and keeping them: the scanner and the address list it feeds.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

void UiApp::renderScanPanel() {
    ImGui::Begin("Scanner");
    const auto typeNames = valueTypeNames();

    ImGui::BeginChild("scan-controls", ImVec2(0, 132.0f), true);
    ImGui::TextDisabled("Scan setup");
    ImGui::SameLine();
    helpMarker("Use Unknown initial for broad baselines, then Changed/Unchanged/Increased/Decreased for narrowing. Exact scans compare against the typed value.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(118.0f);
    ImGui::Combo("Type", &scanTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::Combo("Mode", &scanModeIndex_, scanModeNames, IM_ARRAYSIZE(scanModeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##scan-value", "value or byte pattern", scanText_.data(), scanText_.size());

    if (showScannerFilters_ || ImGui::TreeNodeEx("Region filters", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Writable only", &scanWritableOnly_);
        ImGui::SameLine();
        ImGui::Checkbox("Executable only", &scanExecutableOnly_);
        if (!showScannerFilters_) {
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    const auto type = valueTypeFromIndex(scanTypeIndex_);
    const auto mode = scanModeFromIndex(scanModeIndex_);

    // Changed/Unchanged/Increased/Decreased compare against the previous scan
    // and need no typed value. Requiring one meant the flagship
    // unknown-then-narrow workflow silently did nothing.
    const bool valueless = engine_scan::modeNeedsBaseline(mode) || mode == domain::ScanMode::UnknownInitial;
    auto scanValue = domain::parseScanValue(type, scanText_.data());
    if (!scanValue && valueless && type != domain::ValueType::Bytes) {
        scanValue = domain::parseScanValue(type, "0");
    }

    const auto currentOptions = [this] {
        engine_scan::ScanOptions options;
        options.writableOnly = scanWritableOnly_;
        options.executableOnly = scanExecutableOnly_;
        options.maxResults = static_cast<std::size_t>(std::max(1000, scanMaxResults_));
        options.floatEpsilon = static_cast<double>(scanFloatEpsilon_);
        return options;
    };

    if (ImGui::Button("First scan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process before scanning.");
        } else if (scanValue) {
            services_.scanJob().setOptions(currentOptions());
            services_.scanJob().startFirst(mode, *scanValue);
            if (engine_scan::modeNeedsBaseline(mode)) {
                notifyInfo(std::string(domain::scanModeName(mode)) +
                           " needs something to compare against, so this first scan records a baseline. "
                           "Let the value change, then press Next scan.");
            }
        } else {
            notifyError(type == domain::ValueType::Bytes
                            ? "Enter a byte pattern such as 48 8B ?? 24."
                            : "Scan value is not valid for the selected type.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next scan")) {
        auto previous = services_.scanJob().results();
        if (previous.empty()) {
            notifyError("Run a first scan before filtering with Next scan.");
        } else if (scanValue) {
            services_.scanJob().setOptions(currentOptions());
            services_.scanJob().startNext(mode, *scanValue, std::move(previous));
        } else {
            notifyError("Scan value is not valid for the selected type.");
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
                ImGui::TextUnformatted(domain::toHex(results[i].address).c_str());
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
                    copyText(memoryAddress_.data(), memoryAddress_.size(), domain::toHex(results[i].address));
                    copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(results[i].address));
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
        ImGui::BeginChild("address-editor", ImVec2(0, 126.0f), true);
        ImGui::SetNextItemWidth(210.0f);
        ImGui::InputTextWithHint("Address", "0x7FF...", addAddress_.data(), addAddress_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("Type", &addTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
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
            if (auto address = parseAddress(addAddress_.data())) {
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
            } else {
                ImGui::TextUnformatted(domain::toHex(entry.address).c_str());
            }
            if (entry.chain) {
                ImGui::SameLine();
                ImGui::TextDisabled("(P)");
                if (ImGui::IsItemHovered()) {
                    std::ostringstream tip;
                    tip << domain::narrow(entry.chain->moduleName) << '+'
                        << domain::toHex(entry.chain->moduleOffset);
                    for (const auto offset : entry.chain->offsets) {
                        tip << " -> +0x" << std::hex << offset;
                    }
                    tip << "\n\nTracked as a pointer chain, so it re-resolves when the target restarts.";
                    ImGui::SetTooltip("%s", tip.str().c_str());
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
                copyText(addAddress_.data(), addAddress_.size(), domain::toHex(entry.address));
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
