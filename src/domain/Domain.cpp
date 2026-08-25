#include "domain/Domain.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ire::domain {

namespace {

template <typename T>
std::vector<std::uint8_t> pack(T value) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T>
std::optional<T> readValue(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < sizeof(T)) {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

std::size_t valueTypeSize(ValueType type) {
    switch (type) {
    case ValueType::Int8:
    case ValueType::UInt8: return 1;
    case ValueType::Int16:
    case ValueType::UInt16: return 2;
    case ValueType::Int32:
    case ValueType::UInt32:
    case ValueType::Float: return 4;
    case ValueType::Int64:
    case ValueType::UInt64:
    case ValueType::Double: return 8;
    case ValueType::Bytes: return 0;
    }
    return 0;
}

const char* valueTypeName(ValueType type) {
    switch (type) {
    case ValueType::Int8: return "i8";
    case ValueType::UInt8: return "u8";
    case ValueType::Int16: return "i16";
    case ValueType::UInt16: return "u16";
    case ValueType::Int32: return "i32";
    case ValueType::UInt32: return "u32";
    case ValueType::Int64: return "i64";
    case ValueType::UInt64: return "u64";
    case ValueType::Float: return "f32";
    case ValueType::Double: return "f64";
    case ValueType::Bytes: return "bytes";
    }
    return "unknown";
}

const char* scanModeName(ScanMode mode) {
    switch (mode) {
    case ScanMode::Exact: return "Exact";
    case ScanMode::UnknownInitial: return "Unknown initial";
    case ScanMode::Changed: return "Changed";
    case ScanMode::Unchanged: return "Unchanged";
    case ScanMode::Increased: return "Increased";
    case ScanMode::Decreased: return "Decreased";
    }
    return "Unknown";
}

bool isHardware(BreakpointKind kind) {
    return kind != BreakpointKind::Software;
}

const char* breakpointKindName(BreakpointKind kind) {
    switch (kind) {
    case BreakpointKind::Software: return "Software";
    case BreakpointKind::HardwareExecute: return "Execute";
    case BreakpointKind::HardwareWrite: return "Write";
    case BreakpointKind::HardwareReadWrite: return "Read/write";
    }
    return "Unknown";
}

bool isValidWatchLength(std::uint8_t length) {
    return length == 1 || length == 2 || length == 4 || length == 8;
}

std::optional<ValueType> parseValueType(const std::string& text) {
    const auto value = lower(text);
    for (const auto type : valueTypes()) {
        if (value == valueTypeName(type)) {
            return type;
        }
    }
    return std::nullopt;
}

std::vector<ValueType> valueTypes() {
    return {
        ValueType::Int8, ValueType::UInt8, ValueType::Int16, ValueType::UInt16,
        ValueType::Int32, ValueType::UInt32, ValueType::Int64, ValueType::UInt64,
        ValueType::Float, ValueType::Double, ValueType::Bytes
    };
}

std::string toHex(std::uintptr_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes, bool spaces) {
    std::ostringstream out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (spaces && i != 0) {
            out << ' ';
        }
        out << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return out.str();
}

std::vector<std::uint8_t> parseHexBytes(const std::string& text) {
    std::string hex;
    for (const char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hex.push_back(c);
        }
    }
    if (hex.size() % 2 != 0) {
        hex.insert(hex.begin(), '0');
    }
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned int value{};
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        ss >> value;
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

std::optional<HexPattern> parseHexPattern(const std::string& text) {
    // Accepts "48 8B ?? 24", "488B??24" and "48 8b ? 24". A single '?' stands
    // for a whole wildcard byte, matching the convention used by every other
    // pattern scanner.
    std::string compact;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c)) || c == '?') {
            compact.push_back(c);
        } else {
            return std::nullopt;
        }
    }
    if (compact.empty()) {
        return std::nullopt;
    }

    HexPattern pattern;
    for (std::size_t i = 0; i < compact.size();) {
        if (compact[i] == '?') {
            // Consume "??" as one wildcard byte, or a lone '?' as the same.
            i += (i + 1 < compact.size() && compact[i + 1] == '?') ? 2 : 1;
            pattern.bytes.push_back(0);
            pattern.mask.push_back(0x00);
            continue;
        }
        if (i + 1 >= compact.size() || !std::isxdigit(static_cast<unsigned char>(compact[i + 1]))) {
            return std::nullopt; // odd digit, or a half-wildcard like "A?"
        }
        const auto high = compact[i];
        const auto low = compact[i + 1];
        const auto digit = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
            return static_cast<std::uint8_t>(c - 'A' + 10);
        };
        pattern.bytes.push_back(static_cast<std::uint8_t>((digit(high) << 4) | digit(low)));
        pattern.mask.push_back(0xFF);
        i += 2;
    }
    return pattern;
}

std::optional<std::uintptr_t> parseAddress(const std::string& text) {
    std::string value;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            value.push_back(c);
        }
    }
    if (value.empty()) {
        return std::nullopt;
    }

    // Addresses are always hexadecimal, with or without an 0x prefix. Inferring
    // the base from the digits present made "00400000" parse as decimal and
    // silently resolve to a completely different address.
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value.erase(0, 2);
    }
    if (value.empty() || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return std::nullopt;
    }
    if (value.size() > 16) { // wider than 64 bits
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(std::stoull(value, nullptr, 16));
}

std::optional<ScanValue> parseScanValue(ValueType type, const std::string& text) {
    try {
        ScanValue value;
        value.type = type;
        value.text = text;
        switch (type) {
        case ValueType::Int8: value.bytes = pack(static_cast<std::int8_t>(std::stoll(text))); break;
        case ValueType::UInt8: value.bytes = pack(static_cast<std::uint8_t>(std::stoull(text, nullptr, 0))); break;
        case ValueType::Int16: value.bytes = pack(static_cast<std::int16_t>(std::stoll(text))); break;
        case ValueType::UInt16: value.bytes = pack(static_cast<std::uint16_t>(std::stoull(text, nullptr, 0))); break;
        case ValueType::Int32: value.bytes = pack(static_cast<std::int32_t>(std::stoll(text))); break;
        case ValueType::UInt32: value.bytes = pack(static_cast<std::uint32_t>(std::stoull(text, nullptr, 0))); break;
        case ValueType::Int64: value.bytes = pack(static_cast<std::int64_t>(std::stoll(text))); break;
        case ValueType::UInt64: value.bytes = pack(static_cast<std::uint64_t>(std::stoull(text, nullptr, 0))); break;
        case ValueType::Float: value.bytes = pack(static_cast<float>(std::stof(text))); break;
        case ValueType::Double: value.bytes = pack(static_cast<double>(std::stod(text))); break;
        case ValueType::Bytes: {
            // Wildcards used to be stripped silently, so "90 ?? 90" quietly
            // became the two-byte pattern "90 90".
            auto pattern = parseHexPattern(text);
            if (!pattern) {
                return std::nullopt;
            }
            value.bytes = std::move(pattern->bytes);
            value.mask = std::move(pattern->mask);
            break;
        }
        }
        if (value.bytes.empty()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::string formatValue(ValueType type, const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    switch (type) {
    case ValueType::Int8: if (auto v = readValue<std::int8_t>(bytes)) out << static_cast<int>(*v); break;
    case ValueType::UInt8: if (auto v = readValue<std::uint8_t>(bytes)) out << static_cast<unsigned int>(*v); break;
    case ValueType::Int16: if (auto v = readValue<std::int16_t>(bytes)) out << *v; break;
    case ValueType::UInt16: if (auto v = readValue<std::uint16_t>(bytes)) out << *v; break;
    case ValueType::Int32: if (auto v = readValue<std::int32_t>(bytes)) out << *v; break;
    case ValueType::UInt32: if (auto v = readValue<std::uint32_t>(bytes)) out << *v; break;
    case ValueType::Int64: if (auto v = readValue<std::int64_t>(bytes)) out << *v; break;
    case ValueType::UInt64: if (auto v = readValue<std::uint64_t>(bytes)) out << *v; break;
    case ValueType::Float: if (auto v = readValue<float>(bytes)) out << *v; break;
    case ValueType::Double: if (auto v = readValue<double>(bytes)) out << *v; break;
    case ValueType::Bytes: return bytesToHex(bytes); 
    }
    return out.str();
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrowText(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, narrowText.data(), length, nullptr, nullptr);
    if (!narrowText.empty() && narrowText.back() == '\0') {
        narrowText.pop_back();
    }
    return narrowText;
}

} // namespace ire::domain
