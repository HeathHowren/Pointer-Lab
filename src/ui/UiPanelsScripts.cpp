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
        const auto id = assembler.add({}, "[ENABLE]\n\n[DISABLE]\n");
        editScriptId_ = id;
        scriptName_.fill('\0');
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

    if (ImGui::BeginTable("scripts", 3, denseTableFlags, ImVec2(0, 120.0f))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableHeadersRow();

        for (const auto& script : scripts) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(script.id));

            ImGui::TableNextColumn();
            bool on = script.enabled;
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

            ImGui::TableNextColumn();
            if (ImGui::Selectable(script.name.c_str(), editScriptId_ == script.id)) {
                editScriptId_ = script.id;
                copyText(scriptName_.data(), scriptName_.size(), script.name);
                copyText(scriptSource_.data(), scriptSource_.size(), script.source);
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
    ImGui::SetNextItemWidth(260.0f);
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
    ImGui::SameLine();
    if (ImGui::SmallButton("AOB injection")) {
        copyText(scriptSource_.data(), scriptSource_.size(),
                 assembler.makeTemplate(engine_aa::AutoAssembler::Template::AobInjection, 0, {}, {}));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Code cave")) {
        copyText(scriptSource_.data(), scriptSource_.size(),
                 assembler.makeTemplate(engine_aa::AutoAssembler::Template::CodeCave, 0, {}, {}));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Full injection")) {
        copyText(scriptSource_.data(), scriptSource_.size(),
                 assembler.makeTemplate(engine_aa::AutoAssembler::Template::FullInjection, 0, {}, {}));
    }

    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    ImGui::InputTextMultiline("##script-source", scriptSource_.data(), scriptSource_.size(), ImVec2(-1, -1),
                              editing->enabled ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    ImGui::PopFont();

    ImGui::End();
}

} // namespace ire::ui
