#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ire::domain {

enum class ValueType {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    Bytes
};

enum class ScanMode {
    Exact,
    UnknownInitial,
    Changed,
    Unchanged,
    Increased,
    Decreased
};

struct ProcessInfo {
    std::uint32_t pid{};
    std::wstring name;
};

struct ModuleInfo {
    std::uintptr_t base{};
    std::size_t size{};
    std::wstring name;
    std::wstring path;
};

struct MemoryRegion {
    std::uintptr_t base{};
    std::size_t size{};
    std::uint32_t state{};
    std::uint32_t protect{};
    std::uint32_t type{};
    bool readable{};
    bool writable{};
    bool executable{};
};

struct ScanValue {
    ValueType type{ValueType::Int32};
    std::vector<std::uint8_t> bytes;
    std::string text;
};

struct ScanResult {
    std::uintptr_t address{};
    std::vector<std::uint8_t> previous;
    std::vector<std::uint8_t> current;
};

struct AddressEntry {
    std::uint64_t id{};
    std::uintptr_t address{};
    ValueType type{ValueType::Int32};
    std::vector<std::uint8_t> frozenValue;
    std::string description;
    std::string group;
    std::string hotkey;
    bool frozen{};
};

struct BreakpointInfo {
    std::uintptr_t address{};
    std::uint8_t originalByte{};
    std::uint64_t hitCount{};
    bool enabled{};
    std::string label;
};

struct PointerChain {
    std::wstring moduleName;
    std::uintptr_t moduleBase{};
    std::uintptr_t baseAddress{};
    std::vector<std::ptrdiff_t> offsets;
};

struct Instruction {
    std::uintptr_t address{};
    std::vector<std::uint8_t> bytes;
    std::string text;
};

std::size_t valueTypeSize(ValueType type);
const char* valueTypeName(ValueType type);
const char* scanModeName(ScanMode mode);
std::optional<ValueType> parseValueType(const std::string& text);
std::vector<ValueType> valueTypes();
std::string toHex(std::uintptr_t value);
std::string bytesToHex(const std::vector<std::uint8_t>& bytes, bool spaces = true);
std::vector<std::uint8_t> parseHexBytes(const std::string& text);
std::optional<ScanValue> parseScanValue(ValueType type, const std::string& text);
std::string formatValue(ValueType type, const std::vector<std::uint8_t>& bytes);
std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);

} // namespace ire::domain

