#pragma once

// Shared between the UI translation units.
//
// UiApp is one class split across several files, one group of panels each. These
// helpers used to sit in an anonymous namespace at the top of the single file
// every panel lived in; they are here now so each of those files can still reach
// them. They are `inline` in a named namespace rather than in an anonymous one
// so a file that happens not to use one does not trip /W4 /WX over an unused
// static function.
//
// The common includes live here too, so a panel file needs only this header and
// UiApp.h.

#include "domain/Domain.h"
#include "infra/Logger.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ire::ui {

inline constexpr const char* scanModeNames[] = {"Exact",     "Unknown initial", "Changed",
                                                "Unchanged", "Increased",       "Decreased"};

inline constexpr ImGuiTableFlags denseTableFlags =
    ImGuiTableFlags_BordersInnerV |
    ImGuiTableFlags_BordersOuterH |
    ImGuiTableFlags_RowBg |
    ImGuiTableFlags_Resizable |
    ImGuiTableFlags_Reorderable |
    ImGuiTableFlags_Hideable |
    ImGuiTableFlags_SizingStretchProp;

inline domain::ScanMode scanModeFromIndex(int index) {
    switch (index) {
    case 1: return domain::ScanMode::UnknownInitial;
    case 2: return domain::ScanMode::Changed;
    case 3: return domain::ScanMode::Unchanged;
    case 4: return domain::ScanMode::Increased;
    case 5: return domain::ScanMode::Decreased;
    default: return domain::ScanMode::Exact;
    }
}

inline const char* valueTypeDisplayName(domain::ValueType type) {
    switch (type) {
    case domain::ValueType::Int8:   return "i8 (signed byte)";
    case domain::ValueType::UInt8:  return "u8 (byte)";
    case domain::ValueType::Int16:  return "i16 (short)";
    case domain::ValueType::UInt16: return "u16 (unsigned short)";
    case domain::ValueType::Int32:  return "i32 (int)";
    case domain::ValueType::UInt32: return "u32 (unsigned int)";
    case domain::ValueType::Int64:  return "i64 (long long)";
    case domain::ValueType::UInt64: return "u64 (unsigned long long)";
    case domain::ValueType::Float:  return "f32 (float)";
    case domain::ValueType::Double: return "f64 (double)";
    case domain::ValueType::Bytes:  return "bytes (byte array)";
    }
    return "unknown";
}

inline std::vector<const char*> valueTypeNames() {
    std::vector<const char*> names;
    for (const auto type : domain::valueTypes()) {
        names.push_back(valueTypeDisplayName(type));
    }
    return names;
}

inline domain::ValueType valueTypeFromIndex(int index) {
    const auto types = domain::valueTypes();
    if (index < 0 || static_cast<std::size_t>(index) >= types.size()) {
        return domain::ValueType::Int32;
    }
    return types[static_cast<std::size_t>(index)];
}

inline std::string formatSize(std::size_t size) {
    std::ostringstream out;
    if (size > 1024 * 1024) {
        out << (size / (1024 * 1024)) << " MB";
    } else if (size > 1024) {
        out << (size / 1024) << " KB";
    } else {
        out << size << " B";
    }
    return out.str();
}

inline ImVec4 colorFromBytes(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

inline void statusPill(const char* label, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
    ImGui::SmallButton(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

inline void helpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

inline bool textMatchesFilter(const std::wstring& text, const char* filter) {
    if (!filter || filter[0] == '\0') {
        return true;
    }
    auto haystack = domain::narrow(text);
    auto needle = std::string(filter);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

} // namespace ire::ui
