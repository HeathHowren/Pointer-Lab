// Every byte Pointer Lab has written into the target's code, and a tick box to
// put each one back.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

void UiApp::renderPatchesPanel() {
    ImGui::Begin("Patches", &showPatches_);

    auto& registry = services_.patches();
    const auto patches = registry.patches();

    const auto applied = std::count_if(patches.begin(), patches.end(),
                                       [](const engine_patch::Patch& p) { return p.enabled; });

    statusPill(applied > 0 ? "PATCHED" : "CLEAN",
               applied > 0 ? colorFromBytes(150, 84, 40) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu patch(es), %lld applied", patches.size(), static_cast<long long>(applied));

    if (!patches.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Restore all")) {
            confirmAction("Restore every patch?",
                          "This writes the original bytes back over all " + std::to_string(applied) +
                              " applied patch(es). The records stay in the list so you can re-apply them.",
                          "Restore all", [this]() {
                              if (auto restored = services_.patches().restoreAll(); !restored) {
                                  notifyError(restored.error());
                              } else {
                                  notifyInfo("Restored every patch.");
                              }
                          });
        }
    }

    if (patches.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Nothing has been patched in this session. Assembler patches, and \"replace with code that does "
            "nothing\" from the Access Watch, are recorded here with the bytes they replaced.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Records are kept for as long as you stay attached. Detaching forgets them -- the target keeps "
            "running with whatever is applied at that moment, and Pointer Lab no longer has a handle to change "
            "it, so restore anything you care about before you detach.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // Drift is one ReadProcessMemory per patch. Checked three times a second
    // rather than sixty: code rewriting itself under us is a real thing to
    // notice, but not one that needs frame-rate resolution.
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDriftRefresh_ > std::chrono::milliseconds(300)) {
            lastDriftRefresh_ = now;
            patchDrift_.clear();
            for (const auto& patch : patches) {
                patchDrift_[patch.id] = registry.drifted(patch);
            }
        }
    }

    if (ImGui::BeginTable("patches", 5, denseTableFlags | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, scaled(34.0f));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, scaled(140.0f));
        // Replaced holds the (short) instruction text; Bytes holds the raw hex
        // that would have to be typed back by hand, so it deserves the stretch.
        ImGui::TableSetupColumn("Replaced", ImGuiTableColumnFlags_WidthFixed, scaled(240.0f));
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, scaled(130.0f));
        ImGui::TableHeadersRow();

        for (const auto& patch : patches) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(patch.id));

            ImGui::TableNextColumn();
            bool enabled = patch.enabled;
            if (ImGui::Checkbox("##enabled", &enabled)) {
                if (auto result = registry.setEnabled(patch.id, enabled); !result) {
                    notifyError(result.error());
                }
            }

            ImGui::TableNextColumn();
            ImGui::PushFont(monoFont_, monoFont_->LegacySize);
            ImGui::TextUnformatted(domain::toHex(patch.address).c_str());
            ImGui::PopFont();

            // Something else changed these bytes since we wrote them: the
            // target rewriting its own code, an anti-tamper check, or a write
            // that went around the registry. Silence here would mean the tick
            // box claims a state the memory does not have.
            const auto drift = patchDrift_.find(patch.id);
            if (drift != patchDrift_.end() && drift->second) {
                ImGui::SameLine();
                ImGui::TextColored(colorFromBytes(214, 154, 70), "(!)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "The bytes here are not what Pointer Lab last wrote.\n"
                        "Something else has changed this code. Toggling this patch\n"
                        "will overwrite whatever is there now.");
                }
            }

            ImGui::TableNextColumn();
            if (!patch.originalText.empty()) {
                ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                ImGui::TextUnformatted(patch.originalText.c_str());
                ImGui::PopFont();
            } else {
                ImGui::TextDisabled("%s", patch.description.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::PushFont(monoFont_, monoFont_->LegacySize);
            // Original first: it is the one worth reading, being the thing you
            // would have to type back in by hand if this record were lost.
            ImGui::TextUnformatted(domain::bytesToHex(patch.originalBytes).c_str());
            ImGui::TextDisabled("%s", domain::bytesToHex(patch.patchedBytes).c_str());
            ImGui::PopFont();

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Show")) {
                copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(patch.address));
                showDisassembly_ = true;
                ImGui::SetWindowFocus("Disassembly");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                // Restores first, so removing a record never silently leaves
                // the patch applied with nothing left to undo it.
                if (auto removed = registry.remove(patch.id); !removed) {
                    notifyError("Could not restore that patch, so it was kept in the list: " + removed.error());
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Puts the original bytes back and forgets the patch.");
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace ire::ui
