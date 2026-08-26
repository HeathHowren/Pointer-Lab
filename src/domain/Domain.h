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
    Bytes,
    // Both are variable length, like Bytes: valueTypeSize returns 0 and the
    // length comes from the text being searched for. StringUtf16 is what
    // Windows means by "Unicode" -- two bytes per character, which is how
    // almost every player name and chat line is stored in a Windows game.
    StringAscii,
    StringUtf16
};

enum class ScanMode {
    Exact,
    UnknownInitial,
    Changed,
    Unchanged,
    Increased,
    Decreased,
    // Absolute filters: they test the value as it is now, so unlike the four
    // above they work on a first scan with nothing to compare against.
    ValueBetween,
    BiggerThan,
    SmallerThan,
    // Relative filters. IncreasedBy/DecreasedBy take an exact delta, which is
    // what turns "I took some damage" into "I took exactly 7 damage" and
    // usually finishes a search in one step.
    IncreasedBy,
    DecreasedBy,
    // Compares against the value captured on the *first* scan of the run, not
    // the one before this. That is what makes it useful: a value that went up
    // and came back down is unchanged from the first scan but changed from the
    // previous one.
    SameAsFirst
};

// Pointer width of the attached target.
//
// A 64-bit Pointer Lab can attach to a 32-bit (WOW64) process perfectly well --
// ReadProcessMemory does not care -- but almost everything above the raw read
// does: a pointer chain steps 4 bytes at a time rather than 8, Zydis and
// Keystone need the matching machine mode, and an address formats to 8 hex
// digits rather than 16. Guessing wrong is not a cosmetic error; a pointer scan
// that reads 8-byte pointers out of a 32-bit process finds nothing at all.
enum class Bitness {
    X86,
    X64
};

[[nodiscard]] constexpr std::size_t pointerSize(Bitness bitness) {
    return bitness == Bitness::X86 ? 4u : 8u;
}

[[nodiscard]] constexpr const char* bitnessName(Bitness bitness) {
    return bitness == Bitness::X86 ? "32-bit" : "64-bit";
}

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
    // The second operand, for the modes that take one: the upper bound of
    // ValueBetween, or the delta of IncreasedBy/DecreasedBy. Empty otherwise.
    std::vector<std::uint8_t> bytes2;
    std::string text2;
    // String scans only. Case folding happens at comparison time rather than
    // by folding the needle, because the bytes in the target are not ours to
    // normalise.
    bool caseInsensitive{};
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
    // What was at this address on the *first* scan of the run, carried through
    // every Next scan unchanged. Keeping only `previous` made "same as first
    // scan" impossible to express: a value that rose and fell again is
    // unchanged from the first scan and changed from the one before it, and
    // those are different questions.
    std::vector<std::uint8_t> first;
};

// A path from a static base to a value: read a pointer at the base, add the
// first offset, read a pointer there, add the next, and so on.
//
// The base is normally stored as an offset within its module rather than as an
// absolute address. That is the whole point of a pointer chain: ASLR puts the
// module somewhere different every run, so an absolute base is worthless the
// moment the target restarts.
//
// An empty moduleName means moduleOffset is an absolute address instead. Only
// manual entry produces those -- the scanner always roots a chain in a module --
// and such a chain does not survive a restart. It is allowed because a reader
// following a chain by hand in the hex editor has an absolute base and nothing
// else, and refusing to record it would send them back to writing it on paper.
struct PointerChain {
    std::wstring moduleName;
    std::uintptr_t moduleOffset{};
    std::vector<std::ptrdiff_t> offsets;
    // Where the module sat when the chain was found. For display only; never
    // used to resolve, or the chain would not survive a restart.
    std::uintptr_t moduleBase{};

    [[nodiscard]] bool moduleRooted() const { return !moduleName.empty(); }
    [[nodiscard]] std::uintptr_t scanTimeBase() const { return moduleBase + moduleOffset; }
    // Offsets may legitimately be empty for a manual entry: "the pointer at
    // this address, dereferenced once" is a chain of length one with no
    // trailing offset, and it is the first thing anyone types.
    [[nodiscard]] bool valid() const { return moduleRooted() || moduleOffset != 0; }
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
    // Which register set this actually came from. On a 32-bit target the fields
    // above hold the zero-extended 32-bit registers and r8-r15 are always zero,
    // so the UI has to label them EAX/EIP and hide the extended eight rather
    // than displaying sixteen registers half of which cannot exist.
    Bitness bitness{Bitness::X64};
};

// Register display names for a context, in the order the UI shows them. The
// 32-bit list is deliberately shorter: r8-r15 do not exist in a WOW64 thread.
[[nodiscard]] const char* registerName(Bitness bitness, std::size_t index);
[[nodiscard]] std::size_t registerCount(Bitness bitness);
[[nodiscard]] std::uint64_t registerValue(const RegisterContext& context, std::size_t index);

// How a breakpoint is implemented inside the target.
//
// A software breakpoint replaces an instruction byte with int3. There can be any
// number of them, but the original byte has to be put back and re-armed around
// every hit, and another thread running through the address during that window
// misses it.
//
// A hardware breakpoint uses one of the CPU's four debug registers instead.
// Nothing in the target is modified and nothing is ever disarmed, so the window
// does not exist -- at the cost of there being exactly four. They are also the
// only way to break on data being read or written rather than on execution.
enum class BreakpointKind {
    Software,
    HardwareExecute,
    HardwareWrite,
    HardwareReadWrite
};

[[nodiscard]] bool isHardware(BreakpointKind kind);
[[nodiscard]] const char* breakpointKindName(BreakpointKind kind);
// A debug register watches 1, 2, 4 or 8 bytes, and the address it watches must
// be aligned to that width.
[[nodiscard]] bool isValidWatchLength(std::uint8_t length);

struct BreakpointInfo {
    std::uintptr_t address{};
    // Only meaningful for a software breakpoint; a hardware one never modifies
    // the target.
    std::uint8_t originalByte{};
    std::uint64_t hitCount{};
    bool enabled{};
    std::string label;
    // State of the thread that most recently hit this breakpoint.
    RegisterContext lastHit;
    BreakpointKind kind{BreakpointKind::Software};
    // Bytes watched by a hardware data breakpoint. Always 1 for the rest.
    std::uint8_t length{1};
    // Debug register this breakpoint occupies, 0-3, or -1 when it is software.
    int slot{-1};
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

// 0 for the variable-length types (Bytes and both strings), whose width comes
// from the value being searched for rather than from the type.
std::size_t valueTypeSize(ValueType type);
const char* valueTypeName(ValueType type);
[[nodiscard]] bool isStringType(ValueType type);
const char* scanModeName(ScanMode mode);
std::optional<ValueType> parseValueType(const std::string& text);
std::vector<ValueType> valueTypes();
// Every mode, in the order the UI offers them.
std::vector<ScanMode> scanModes();
// True for the modes that take a second operand: the upper bound of
// ValueBetween, the delta of IncreasedBy/DecreasedBy.
[[nodiscard]] bool modeUsesSecondValue(ScanMode mode);
// True when the mode compares against a typed value at all. The rest compare
// against what the previous scan saw, and asking for a value would be asking
// for something that cannot be supplied.
[[nodiscard]] bool modeUsesValue(ScanMode mode);
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

