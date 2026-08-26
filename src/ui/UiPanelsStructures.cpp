// The structure dissector: one layout laid over several objects at once, so the
// fields that describe an object can be told apart from the ones that happen to
// be the same in both.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

namespace {

// Grey for a field that reads the same at every address. Not hidden -- padding
// and shared state are worth seeing -- but pushed back, so the fields that
// differ are the ones the eye lands on.
ImVec4 identicalFieldColor() {
    return colorFromBytes(126, 138, 152);
}

} // namespace

void UiApp::dissect(std::uintptr_t address) {
    auto& dissector = services_.dissector();
    if (structureId_ == 0 || !dissector.find(structureId_)) {
        structureId_ = dissector.add({});
    }

    // Appended rather than replacing, because the second address is the whole
    // point: one object tells you what is there, two tell you which part of it
    // is the object.
    const auto text = domain::toHex(address);
    std::string existing = structureAddresses_.data();
    if (existing.find(text) == std::string::npos) {
        if (!existing.empty()) {
            existing += ", ";
        }
        existing += text;
        copyText(structureAddresses_.data(), structureAddresses_.size(), existing);
    }

    showStructures_ = true;
    ImGui::SetWindowFocus("Structures");
    notifyInfo("Added " + text + " to the structure view. Add a second instance of the same kind of object "
               "and press Guess: the fields that differ between them are the ones worth naming.");
}

std::vector<std::uintptr_t> UiApp::structureAddressList() {
    std::vector<std::uintptr_t> addresses;
    std::string text = structureAddresses_.data();
    std::string token;
    const auto flush = [&]() {
        if (token.empty()) {
            return;
        }
        if (const auto address = resolveAddress(token.c_str())) {
            addresses.push_back(*address);
        }
        token.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';') {
            flush();
        } else if (c != ' ' && c != '\t') {
            token.push_back(c);
        }
    }
    flush();

    if (addresses.size() > engine_struct::Dissector::maxAddresses) {
        addresses.resize(engine_struct::Dissector::maxAddresses);
    }
    return addresses;
}

void UiApp::renderStructuresPanel() {
    ImGui::Begin("Structures", &showStructures_);

    auto& dissector = services_.dissector();
    const auto structures = dissector.structures();

    statusPill(structures.empty() ? "EMPTY" : "READY",
               structures.empty() ? colorFromBytes(63, 75, 88) : colorFromBytes(30, 111, 96));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu structure(s)", structures.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("New")) {
        structureId_ = dissector.add({});
    }
    ImGui::SameLine();
    helpMarker(
        "A game does not have one player, it has an array of them. Once you know health is at +0x9C of "
        "something, the same +0x9C answers the same question for every enemy on the map.\n\n"
        "Put two addresses of the same kind of object side by side and press Guess. Fields that read the "
        "same in both are greyed: those are padding, or state the two objects share. The ones that differ "
        "are what actually describes an object, and they are where the names are worth writing.\n\n"
        "The types are a guess from the bytes -- a value in a mapped page is called a pointer, a plausible "
        "exponent is called a float. Correct them as you learn what they really are; the layout is saved "
        "with the project.");

    if (structures.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Nothing defined yet. Press New, or right-click an address in the Address List, the scan "
            "results or the hex editor and choose \"Dissect this\".");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "The usual way in is the Access Watch: when it names a register as the likely structure base, "
            "that register's value is the start of an object, and this is where you find out what is in it.");
        ImGui::End();
        return;
    }

    // Structure picker.
    ImGui::Separator();
    if (structureId_ == 0 || !dissector.find(structureId_)) {
        structureId_ = structures.front().id;
    }
    const auto current = dissector.find(structureId_);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##structure", current ? current->name.c_str() : "")) {
        for (const auto& structure : structures) {
            const bool selected = structure.id == structureId_;
            if (ImGui::Selectable(structure.name.c_str(), selected)) {
                structureId_ = structure.id;
                copyText(structureName_.data(), structureName_.size(), structure.name);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##structure-name", "rename", structureName_.data(), structureName_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rename")) {
        if (auto renamed = dissector.rename(structureId_, structureName_.data()); !renamed) {
            notifyError(renamed.error());
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
        const auto id = structureId_;
        const auto label = current ? current->name : std::string();
        confirmAction("Remove this structure?",
                      "\"" + label + "\" and everything named in it is forgotten. The target is not touched.",
                      "Remove", [this, id] {
                          if (auto removed = services_.dissector().remove(id); !removed) {
                              notifyError(removed.error());
                          } else {
                              structureId_ = 0;
                          }
                      });
    }

    // Addresses and the guess pass.
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::InputTextWithHint("##structure-addresses", "addresses, comma separated",
                             structureAddresses_.data(), structureAddresses_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##structure-size", &structureSize_, 0, 0, ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many bytes of the object to read, in hex.");
    }
    ImGui::SameLine();
    const auto addresses = structureAddressList();
    ImGui::BeginDisabled(addresses.empty());
    if (ImGui::Button("Guess")) {
        auto guessed = dissector.guess(structureId_, addresses, static_cast<std::size_t>(structureSize_));
        if (!guessed) {
            notifyError(guessed.error());
        } else {
            editFieldOffset_.reset();
            notifyInfo("Filled in " + std::to_string(guessed.value()) +
                       " field(s) from what is at " + std::to_string(addresses.size()) +
                       " address(es). The types are a guess -- correct them as you find out what they are.");
        }
    }
    ImGui::EndDisabled();

    if (addresses.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Type one or more addresses above. Anything the address boxes accept works here "
                           "too: a plain address, a symbol, or game.exe+0x4A2C10.");
        ImGui::End();
        return;
    }

    auto snapshot = dissector.read(structureId_, addresses);
    if (!snapshot) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", snapshot.error().c_str());
        ImGui::End();
        return;
    }
    if (snapshot.value().rows.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("No fields yet. Press Guess to fill the layout in from what is actually there, "
                           "then rename and retype the ones that matter.");
        ImGui::End();
        return;
    }

    for (const auto index : snapshot.value().unreadable) {
        ImGui::TextColored(colorFromBytes(214, 154, 70), "%s could not be read.",
                           domain::toHex(addresses[index]).c_str());
    }

    renderStructureFieldEditor();

    ImGui::Separator();

    const int columns = 3 + static_cast<int>(addresses.size());
    if (ImGui::BeginTable("structure", columns, denseTableFlags | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(3, 1);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        for (const auto address : addresses) {
            ImGui::TableSetupColumn(domain::toHex(address).c_str(), ImGuiTableColumnFlags_WidthStretch);
        }
        ImGui::TableHeadersRow();

        for (const auto& row : snapshot.value().rows) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(row.field.offset));

            const bool grey = row.identical && addresses.size() > 1;
            if (grey) {
                ImGui::PushStyleColor(ImGuiCol_Text, identicalFieldColor());
            }

            ImGui::TableNextColumn();
            ImGui::PushFont(monoFont_, monoFont_->LegacySize);
            ImGui::TextUnformatted(
                ("+" + domain::toHex(static_cast<std::uintptr_t>(row.field.offset))).c_str());
            ImGui::PopFont();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.field.name.c_str());
            if (ImGui::IsItemHovered() && addresses.size() > 1) {
                ImGui::SetTooltip(row.identical
                                      ? "The same at every address: padding, or state these objects share."
                                      : "Differs between these objects, so it is part of what describes one.");
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(domain::valueTypeName(row.field.type));

            for (const auto& cell : row.cells) {
                ImGui::TableNextColumn();
                if (!cell.read) {
                    ImGui::TextDisabled("--");
                    continue;
                }
                ImGui::PushFont(monoFont_, monoFont_->LegacySize);
                ImGui::TextUnformatted(cell.text.c_str());
                ImGui::PopFont();
                if (!cell.annotation.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(staticAddressColor(), "%s", cell.annotation.c_str());
                }
            }

            if (grey) {
                ImGui::PopStyleColor();
            }

            // The row menu hangs off the name cell's whole row, which is where
            // people click. Retyping a field is the commonest thing done here,
            // because the guess is only ever a guess.
            if (ImGui::BeginPopupContextItem("##field-menu")) {
                renderStructureFieldMenu(row.field, addresses);
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void UiApp::renderStructureFieldEditor() {
    if (!editFieldOffset_) {
        return;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Editing +%s",
                        domain::toHex(static_cast<std::uintptr_t>(*editFieldOffset_)).c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##field-name", "name", editFieldName_.data(), editFieldName_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    const auto typeNames = valueTypeNames();
    ImGui::Combo("##field-type", &editFieldTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));

    // Only the variable-width types take a length; for the rest it comes from
    // the type and letting anyone type one would invite a field claiming a
    // width its type cannot have.
    const auto type = valueTypeFromIndex(editFieldTypeIndex_);
    const bool variable = domain::valueTypeSize(type) == 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!variable);
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("##field-length", &editFieldLength_, 0, 0);
    ImGui::EndDisabled();
    if (!variable) {
        editFieldLength_ = static_cast<int>(domain::valueTypeSize(type));
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        domain::StructureField field;
        field.offset = *editFieldOffset_;
        field.type = type;
        field.length = static_cast<std::size_t>(std::max(editFieldLength_, 0));
        field.name = editFieldName_.data();
        if (auto applied = services_.dissector().setField(structureId_, field); !applied) {
            notifyError(applied.error());
        } else {
            editFieldOffset_.reset();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        editFieldOffset_.reset();
    }
}

void UiApp::renderStructureFieldMenu(const domain::StructureField& field,
                                     const std::vector<std::uintptr_t>& addresses) {
    ImGui::TextDisabled("%s at +%s", field.name.c_str(),
                        domain::toHex(static_cast<std::uintptr_t>(field.offset)).c_str());
    ImGui::Separator();

    if (ImGui::BeginMenu("Type")) {
        for (const auto type : domain::valueTypes()) {
            if (domain::valueTypeSize(type) == 0) {
                // Bytes and the strings need a length, which a menu cannot ask
                // for. They are set from the rename box below instead.
                continue;
            }
            if (ImGui::MenuItem(valueTypeDisplayName(type), nullptr, type == field.type)) {
                auto changed = field;
                changed.type = type;
                if (auto applied = services_.dissector().setField(structureId_, changed); !applied) {
                    notifyError(applied.error());
                }
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Edit...")) {
        editFieldOffset_ = field.offset;
        copyText(editFieldName_.data(), editFieldName_.size(), field.name);
        const auto types = domain::valueTypes();
        const auto found = std::find(types.begin(), types.end(), field.type);
        editFieldTypeIndex_ = found == types.end() ? 4 : static_cast<int>(found - types.begin());
        editFieldLength_ = static_cast<int>(field.size());
        ImGui::CloseCurrentPopup();
    }

    if (ImGui::MenuItem("Remove field")) {
        if (auto removed = services_.dissector().removeField(structureId_, field.offset); !removed) {
            notifyError(removed.error());
        }
    }

    ImGui::Separator();

    // The point of naming a field is to watch it, so the two things that turn a
    // named field into something usable are here rather than three panels away.
    if (ImGui::MenuItem("Add every instance to the address list")) {
        std::size_t added = 0;
        for (const auto address : addresses) {
            services_.addressList().add(address + static_cast<std::uintptr_t>(field.offset), field.type,
                                        field.name, "Structure");
            ++added;
        }
        notifyInfo("Added " + std::to_string(added) + " entr(ies) for " + field.name + ".");
    }
    if (!addresses.empty() && ImGui::MenuItem("Show the first one in the hex editor")) {
        gotoMemory(addresses.front() + static_cast<std::uintptr_t>(field.offset));
    }
    if (!addresses.empty() && ImGui::MenuItem("Find out what writes to the first one")) {
        beginAccessWatch(addresses.front() + static_cast<std::uintptr_t>(field.offset), field.type, true);
    }
}

} // namespace ire::ui
