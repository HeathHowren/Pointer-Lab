// Names for addresses: the symbol table, and the resolver every address box in
// the tool goes through.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

infra::Result<std::uintptr_t> UiApp::resolveAddressOrExplain(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return infra::Result<std::uintptr_t>::fail("Enter an address, or a name such as client.dll+0x4A2C10.");
    }
    return services_.symbols().resolve(services_.session(), text);
}

std::optional<std::uintptr_t> UiApp::resolveAddress(const char* text) {
    auto resolved = resolveAddressOrExplain(text);
    if (!resolved) {
        return std::nullopt;
    }
    return resolved.value();
}

void UiApp::renderSymbolsPanel() {
    ImGui::Begin("Symbols", &showSymbols_);

    auto& table = services_.symbols();
    const auto symbols = table.symbols();

    statusPill(symbols.empty() ? "NONE" : "NAMED",
               symbols.empty() ? colorFromBytes(63, 75, 88) : colorFromBytes(30, 111, 96));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu symbol(s)", symbols.size());

    ImGui::Separator();
    ImGui::TextWrapped(
        "A name you can type anywhere an address is asked for. Every address box in Pointer Lab accepts "
        "these, so a value found once can be referred to by name for the rest of the session.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##symbol-name", "name", symbolName_.data(), symbolName_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-90.0f);
    const bool submitted =
        ImGui::InputTextWithHint("##symbol-expression", "client.dll+0x4A2C10", symbolExpression_.data(),
                                 symbolExpression_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Define") || submitted) {
        auto defined = table.define(services_.session(), symbolName_.data(), symbolExpression_.data());
        if (!defined) {
            notifyError(defined.error());
        } else {
            notifyInfo(std::string(symbolName_.data()) + " = " + domain::toHex(defined.value()) + ".");
            symbolName_.fill('\0');
            symbolExpression_.fill('\0');
        }
    }

    ImGui::TextDisabled("Accepted: a module (client.dll), a module and offset (client.dll+0x4A2C10), an export "
                        "(kernel32.LoadLibraryW), another symbol, or a plain hexadecimal address -- each "
                        "followed by any number of +/- offsets.");

    if (symbols.empty()) {
        ImGui::End();
        return;
    }

    ImGui::Separator();
    if (ImGui::BeginTable("symbols", 4, denseTableFlags)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Definition", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        for (const auto& symbol : symbols) {
            ImGui::TableNextRow();
            ImGui::PushID(symbol.name.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(symbol.name.c_str());

            ImGui::TableNextColumn();
            ImGui::PushFont(monoFont_, monoFont_->LegacySize);
            if (engine_symbols::SymbolTable::isStatic(services_.session(), symbol.address)) {
                ImGui::TextColored(staticAddressColor(), "%s", domain::toHex(symbol.address).c_str());
            } else {
                ImGui::TextUnformatted(domain::toHex(symbol.address).c_str());
            }
            ImGui::PopFont();

            ImGui::TableNextColumn();
            if (symbol.expression.empty()) {
                ImGui::TextDisabled("(fixed address)");
            } else {
                ImGui::TextUnformatted(symbol.expression.c_str());
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Re-resolve")) {
                // Worth having as a button rather than doing it silently every
                // frame: re-resolving reads the target, and a symbol whose
                // module has been unloaded should report that rather than
                // quietly reverting to a stale address.
                if (symbol.expression.empty()) {
                    notifyError(symbol.name + " was defined as a fixed address, so there is nothing to re-resolve.");
                } else if (auto again = table.define(services_.session(), symbol.name, symbol.expression); !again) {
                    notifyError(again.error());
                } else {
                    notifyInfo(symbol.name + " = " + domain::toHex(again.value()) + ".");
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                table.undefine(symbol.name);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace ire::ui
