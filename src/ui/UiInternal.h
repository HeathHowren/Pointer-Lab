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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ire::ui {

inline std::vector<const char*> scanModeNames() {
    std::vector<const char*> names;
    for (const auto mode : domain::scanModes()) {
        names.push_back(domain::scanModeName(mode));
    }
    return names;
}

inline domain::ScanMode scanModeFromIndex(int index) {
    const auto modes = domain::scanModes();
    if (index < 0 || static_cast<std::size_t>(index) >= modes.size()) {
        return domain::ScanMode::Exact;
    }
    return modes[static_cast<std::size_t>(index)];
}

inline constexpr ImGuiTableFlags denseTableFlags =
    ImGuiTableFlags_BordersInnerV |
    ImGuiTableFlags_BordersOuterH |
    ImGuiTableFlags_RowBg |
    ImGuiTableFlags_Resizable |
    ImGuiTableFlags_Reorderable |
    ImGuiTableFlags_Hideable |
    ImGuiTableFlags_SizingStretchProp;

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
    case domain::ValueType::StringAscii:  return "str (text)";
    case domain::ValueType::StringUtf16:  return "wstr (text, 2 bytes/char)";
    }
    return "unknown";
}

// Green for an address inside a loaded module, because such an address is at
// the same module+offset every run. This is the single most useful thing the
// results table can say about a row: it is the difference between an address
// worth writing down and one that is wherever the allocator put it today.
inline ImVec4 staticAddressColor() {
    return ImVec4(0.47f, 0.78f, 0.55f, 1.0f);
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
    // snprintf rather than a stringstream: the regions table calls this once
    // per visible row per frame.
    char buffer[32];
    if (size > 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu MB", static_cast<unsigned long long>(size / (1024 * 1024)));
    } else if (size > 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu KB", static_cast<unsigned long long>(size / 1024));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(size));
    }
    return buffer;
}

inline ImVec4 colorFromBytes(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// Every fixed width in this UI was picked by eye at 96 DPI. ImGui scales the
// font and the style metrics for a high-DPI monitor but knows nothing about a
// column width a panel passed it, so those go through here: without it, a 150%
// display draws a wider "0x00007FF7A2B41000" into the same 140 pixels.
inline float scaled(float pixels) {
    return pixels * ImGui::GetStyle().FontScaleDpi;
}

// How wide an input plus its trailing label comes out.
inline float labeledWidth(float itemWidth, const char* label) {
    return itemWidth + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
}

// SameLine, but only while an item that wide still fits on the current line.
// Panels dock to whatever width the layout gives them, and a fixed chain of
// SameLine calls simply runs a control row off the right edge -- there is no
// scrollbar on a window's own contents, so what fell off could not be reached
// at all.
inline void sameLineIfRoom(float width) {
    // Called just after an item, so the cursor is already at the start of the
    // next line: its x plus the remaining width is the content region's right
    // edge, which is what the previous item has to be measured against.
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right) {
        ImGui::SameLine();
    }
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

// For a table cell whose text can be wider than its column: the column clips
// the text in place, and the full string is a hover away instead of lost.
inline void cellText(const char* text) {
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(text);
    if (ImGui::CalcTextSize(text).x > avail && ImGui::BeginItemTooltip()) {
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
