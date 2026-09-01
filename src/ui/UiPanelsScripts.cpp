// Auto-assembler scripts: an injection written down as a description of itself
// rather than performed by hand.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

void UiApp::newScriptFromAddress(std::uintptr_t address, const std::vector<std::uint8_t>& bytes,
                                 int templateShape) {
    std::string moduleName;
    if (const auto module = engine_symbols::SymbolTable::moduleAt(services_.session(), address)) {
        moduleName = domain::narrow(module->name);
    }

    const auto source = services_.autoAssembler().makeTemplate(
        static_cast<engine_aa::AutoAssembler::Template>(templateShape), address, bytes, moduleName);

    const auto id = services_.autoAssembler().add("Injection at " + domain::toHex(address), source);
    editScriptId_ = id;
    copyText(scriptName_.data(), scriptName_.size(), "Injection at " + domain::toHex(address));
    copyText(scriptSource_.data(), scriptSource_.size(), source);
    showScripts_ = true;
    ImGui::SetWindowFocus("Scripts");
    notifyInfo("Started a script from " + domain::toHex(address) +
               ". Read it before turning it on -- the template leaves a gap where your code goes.");
}

void UiApp::renderScriptsPanel() {
    ImGui::Begin("Scripts", &showScripts_);

    auto& assembler = services_.autoAssembler();
    const auto scripts = assembler.scripts();
    const auto enabled = std::count_if(scripts.begin(), scripts.end(),
                                       [](const engine_aa::Script& s) { return s.enabled; });

    statusPill(enabled > 0 ? "ACTIVE" : "IDLE",
               enabled > 0 ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu script(s), %lld on", scripts.size(), static_cast<long long>(enabled));
    ImGui::SameLine();
    if (ImGui::SmallButton("New")) {
        // A blank name renders as an empty row that nothing distinguishes from
        // whitespace, and the user is left wondering why New "did not work".
        const auto id = assembler.add("New script", "[ENABLE]\n\n[DISABLE]\n");
        editScriptId_ = id;
        copyText(scriptName_.data(), scriptName_.size(), "New script");
        copyText(scriptSource_.data(), scriptSource_.size(), "[ENABLE]\n\n[DISABLE]\n");
    }
    ImGui::SameLine();
    helpMarker(
        "A script has two halves. [ENABLE] patches the target; [DISABLE] puts it back.\n\n"
        "Directives:\n"
        "  aobscanmodule(name, module, pattern)  find bytes in a module and name where they start\n"
        "  alloc(name, size, near)               allocate; with a third argument, within reach of a jmp\n"
        "  label(name) / name:                   declare and define a label\n"
        "  registersymbol / unregistersymbol     publish a name to the Symbols panel\n"
        "  dealloc(name)                         free an allocation\n"
        "  define(name, text)                    textual substitution\n"
        "  assert(name, bytes)                   refuse to run unless those bytes are really there\n"
        "  db / dw / dd / dq                     raw data\n\n"
        "A name that already has a value -- an allocation, or the result of a scan -- acts as an origin: "
        "what follows it is assembled at that address. So does any address expression: \"7FF612340000:\" "
        "and \"game.exe+8A3F1:\" both work.");

    if (scripts.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "A script is an injection written down: \"find these bytes, allocate a cave within reach of them, "
            "put this code in it, and jump to it from there\". Written that way it can be read, checked, kept "
            "and re-run after the target restarts, none of which is true of the same work done by hand.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Press New, or press Script on a row in the Access Watch to start from a template already filled "
            "in with the instruction you found, the bytes there and the module they belong to.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // Without ScrollY the fixed 120px was a minimum, so at ~10 scripts the
    // table grew unbounded and crushed the editor beneath it. The clipper
    // and scrolling keep it bounded.
    // Four rows and a header, in whatever a row currently measures, rather than
    // a flat 120 pixels: on a scaled display that was two and a bit rows.
    if (ImGui::BeginTable("scripts", 3, denseTableFlags | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 5.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, scaled(34.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, scaled(140.0f));
        ImGui::TableHeadersRow();

        for (const auto& script : scripts) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(script.id));

            ImGui::TableNextColumn();
            bool on = script.enabled;
            // A dirty editor buffer means Check-and-then-enable would run the
            // saved text, not the text the user just Check-ed. Refusing the
            // toggle is more honest than quietly running the wrong version.
            const bool dirty = editScriptId_ == script.id && !on && script.source != scriptSource_.data();
            ImGui::BeginDisabled(dirty);
            if (ImGui::Checkbox("##enabled", &on)) {
                if (auto result = assembler.setEnabled(script.id, on); !result) {
                    notifyError(result.error());
                } else if (on) {
                    // The notes are the answer to "what did it actually do?",
                    // which matters far more than "it worked" when the whole
                    // point of the script is that it found the address itself.
                    const auto notes = assembler.lastNotes(script.id);
                    std::string message = script.name + " is on.";
                    for (const auto& note : notes) {
                        message += "\n  " + note;
                    }
                    notifyInfo(message);
                } else {
                    notifyInfo(script.name + " is off and the target is back as it was.");
                }
            }
            ImGui::EndDisabled();
            if (dirty && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Save your edits first. Turning this on runs the saved text, not what is in "
                                  "the editor.");
            }

            ImGui::TableNextColumn();
            if (ImGui::Selectable(script.name.c_str(), editScriptId_ == script.id)) {
                const bool wouldLose = editScriptId_ != 0 && editScriptId_ != script.id;
                bool discard = true;
                if (wouldLose) {
                    if (const auto previous = assembler.find(editScriptId_)) {
                        discard = previous->source == scriptSource_.data();
                    }
                }
                const auto load = [this, id = script.id, name = script.name, src = script.source] {
                    editScriptId_ = id;
                    copyText(scriptName_.data(), scriptName_.size(), name);
                    copyText(scriptSource_.data(), scriptSource_.size(), src);
                };
                if (!discard) {
                    confirmAction("Discard unsaved edits?",
                                  "The script you were editing has unsaved changes. Loading another script "
                                  "replaces the editor text; there is no undo.",
                                  "Discard and load", load);
                } else {
                    load();
                }
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                const auto id = script.id;
                const auto label = script.name;
                confirmAction("Remove this script?",
                              "\"" + label +
                                  "\" is removed from the list. If it is on, it is turned off first and "
                                  "everything it changed in the target is put back.",
                              "Remove", [this, id] {
                                  if (auto removed = services_.autoAssembler().remove(id); !removed) {
                                      notifyError("Kept in the list: " + removed.error());
                                  } else if (editScriptId_ == id) {
                                      editScriptId_ = 0;
                                  }
                              });
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (editScriptId_ == 0) {
        ImGui::TextDisabled("Select a script to edit it.");
        ImGui::End();
        return;
    }

    const auto editing = assembler.find(editScriptId_);
    if (!editing) {
        editScriptId_ = 0;
        ImGui::End();
        return;
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(scaled(260.0f));
    ImGui::InputTextWithHint("##script-name", "name", scriptName_.data(), scriptName_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(editing->enabled);
    if (ImGui::Button("Save")) {
        if (auto updated = assembler.update(editScriptId_, scriptName_.data(), scriptSource_.data());
            !updated) {
            notifyError(updated.error());
        } else {
            notifyInfo("Saved.");
        }
    }
    ImGui::EndDisabled();
    if (editing->enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Turn the script off first. The section that undoes it is part of the text you are "
                          "about to replace.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Check")) {
        // Compiles without writing anything. Reading a script critically before
        // running it is only possible if you can see what it worked out.
        auto checked = assembler.check(scriptSource_.data(), true);
        if (!checked) {
            notifyError(checked.error());
        } else {
            std::string message = "The [ENABLE] section compiles.";
            for (const auto& note : checked.value().notes) {
                message += "\n  " + note;
            }
            std::size_t patched{};
            std::size_t written{};
            for (const auto& block : checked.value().blocks) {
                (block.intoAllocation ? written : patched) += block.bytes.size();
            }
            message += "\n  " + std::to_string(patched) + " byte(s) over the target's own code, " +
                       std::to_string(written) + " into allocated memory.";
            notifyInfo(message);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Templates:");
    const auto applyTemplate = [&, this](engine_aa::AutoAssembler::Template shape) {
        auto text = assembler.makeTemplate(shape, 0, {}, {});
        // Templates blow away everything in the editor. The same discard prompt
        // the row selector uses applies here for the same reason.
        const bool dirty = editing && editing->source != scriptSource_.data();
        if (dirty) {
            confirmAction("Discard unsaved edits?",
                          "The template replaces the entire editor text. There is no undo.",
                          "Discard and load template",
                          [this, text] { copyText(scriptSource_.data(), scriptSource_.size(), text); });
        } else {
            copyText(scriptSource_.data(), scriptSource_.size(), text);
        }
    };
    ImGui::SameLine();
    if (ImGui::SmallButton("AOB injection")) {
        applyTemplate(engine_aa::AutoAssembler::Template::AobInjection);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Code cave")) {
        applyTemplate(engine_aa::AutoAssembler::Template::CodeCave);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Full injection")) {
        applyTemplate(engine_aa::AutoAssembler::Template::FullInjection);
    }

    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    ImGui::InputTextMultiline("##script-source", scriptSource_.data(), scriptSource_.size(), ImVec2(-1, -1),
                              editing->enabled ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    ImGui::PopFont();

    ImGui::End();
}

} // namespace ire::ui
