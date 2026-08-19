#pragma once

#include <algorithm>
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
    // Parallel to bytes for wildcard patterns: 0xFF compares, 0x00 ignores.
    // Empty means every byte is compared.
    std::vector<std::uint8_t> mask;
    std::string text;
};

// A byte pattern with optional '?' / '??' wildcards, e.g. "48 8B ?? 24".
struct HexPattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;

    [[nodiscard]] bool hasWildcards() const {
        return std::any_of(mask.begin(), mask.end(), [](std::uint8_t m) { return m == 0; });
    }
};

struct ScanResult {
    std::uintptr_t address{};
    std::vector<std::uint8_t> previous;
    std::vector<std::uint8_t> current;
};

// A path from a static base to a value: read a pointer at the base, add the
// first offset, read a pointer there, add the next, and so on.
//
// The base is stored as an offset within its module rather than as an absolute
// address. That is the whole point of a pointer chain: ASLR puts the module
// somewhere different every run, so an absolute base is worthless the moment
// the target restarts.
struct PointerChain {
    std::wstring moduleName;
    std::uintptr_t moduleOffset{};
    std::vector<std::ptrdiff_t> offsets;
    // Where the module sat when the chain was found. For display only; never
    // used to resolve, or the chain would not survive a restart.
    std::uintptr_t moduleBase{};

    [[nodiscard]] std::uintptr_t scanTimeBase() const { return moduleBase + moduleOffset; }
    [[nodiscard]] bool valid() const { return !moduleName.empty() && !offsets.empty(); }
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
    // When set, address is recomputed from this chain instead of being fixed,
    // so the entry keeps pointing at the right value after the target restarts.
    std::optional<PointerChain> chain;
    // False once a chain has stopped resolving, so the UI can say the entry is
    // stale rather than showing whatever happens to live at a dead address.
    bool resolved{true};
};

// Register state captured when a breakpoint fires. Deliberately free of any
// Windows types so the UI and the domain layer can pass it around.
struct RegisterContext {
    std::uint64_t rip{};
    std::uint64_t rsp{};
    std::uint64_t rbp{};
    std::uint64_t rax{};
    std::uint64_t rbx{};
    std::uint64_t rcx{};
    std::uint64_t rdx{};
    std::uint64_t rsi{};
    std::uint64_t rdi{};
    std::uint64_t r8{};
    std::uint64_t r9{};
    std::uint64_t r10{};
    std::uint64_t r11{};
    std::uint64_t r12{};
    std::uint64_t r13{};
    std::uint64_t r14{};
    std::uint64_t r15{};
    std::uint32_t eflags{};
    std::uint32_t threadId{};
    bool captured{};
};

struct BreakpointInfo {
    std::uintptr_t address{};
    std::uint8_t originalByte{};
    std::uint64_t hitCount{};
    bool enabled{};
    std::string label;
    // State of the thread that most recently hit this breakpoint.
    RegisterContext lastHit;
};

struct Instruction {
    std::uintptr_t address{};
    std::vector<std::uint8_t> bytes;
    std::string text;
    // Absolute destination of a call/jmp/jcc with a computed target, so the UI
    // can offer to follow it. 0 when the instruction does not branch, or
    // branches somewhere only known at runtime (e.g. "jmp rax").
    std::uintptr_t branchTarget{};
    // False when the bytes did not decode and are shown as data instead.
    bool valid{true};
};

std::size_t valueTypeSize(ValueType type);
const char* valueTypeName(ValueType type);
const char* scanModeName(ScanMode mode);
std::optional<ValueType> parseValueType(const std::string& text);
std::vector<ValueType> valueTypes();
std::string toHex(std::uintptr_t value);
std::string bytesToHex(const std::vector<std::uint8_t>& bytes, bool spaces = true);
std::vector<std::uint8_t> parseHexBytes(const std::string& text);
// Returns nullopt when the text is not a well-formed byte pattern. Unlike
// parseHexBytes, which silently discards anything that is not a hex digit,
// this reports bad input and understands '?' wildcards.
std::optional<HexPattern> parseHexPattern(const std::string& text);
std::optional<std::uintptr_t> parseAddress(const std::string& text);
std::optional<ScanValue> parseScanValue(ValueType type, const std::string& text);
std::string formatValue(ValueType type, const std::vector<std::uint8_t>& bytes);
std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);

} // namespace ire::domain

