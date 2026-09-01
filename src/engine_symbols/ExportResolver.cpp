#include "engine_symbols/ExportResolver.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace ire::engine_symbols {

namespace {

using Address = infra::Result<std::uintptr_t>;
using Entries = infra::Result<std::vector<ExportEntry>>;

// PE constants, spelled out rather than pulled from Windows.h.
//
// This file parses a byte buffer read out of another process; it makes no OS
// calls, so under the layering in docs/architecture.md it has no business
// including Windows.h. The values are fixed by the PE/COFF specification and
// cannot drift.
constexpr std::uint16_t dosSignature = 0x5A4D;      // "MZ"
constexpr std::uint32_t ntSignature = 0x00004550;   // "PE\0\0"
constexpr std::uint16_t optionalMagic32 = 0x010B;   // PE32
constexpr std::uint16_t optionalMagic64 = 0x020B;   // PE32+
constexpr std::size_t lfanewOffset = 0x3C;
constexpr std::size_t fileHeaderSize = 20;
// Offset of the data directory array within the optional header. PE32+ drops
// BaseOfData (4 bytes) and widens four fields from 4 to 8 bytes, which is where
// the 16-byte difference comes from.
constexpr std::size_t dataDirectoryOffset32 = 96;
constexpr std::size_t dataDirectoryOffset64 = 112;

// A forwarder chain longer than this is a loop. Real chains are one or two
// hops: kernel32 -> KERNELBASE, occasionally onward to NTDLL.
constexpr int maxForwarderDepth = 8;

template <typename T>
bool readScalar(const std::vector<std::uint8_t>& buffer, std::size_t offset, T& out) {
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(&out, buffer.data() + offset, sizeof(T));
    return true;
}

std::wstring lowerW(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// "kernel32", "KERNEL32.DLL" and "kernel32.dll" all name the same module.
bool sameModule(std::wstring a, std::wstring b) {
    a = lowerW(std::move(a));
    b = lowerW(std::move(b));
    const auto stripDll = [](std::wstring& name) {
        constexpr std::wstring_view suffix = L".dll";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.erase(name.size() - suffix.size());
        }
    };
    stripDll(a);
    stripDll(b);
    return a == b;
}

// Reads a NUL-terminated ASCII string out of the target. Names in an export
// directory have no length prefix, so this reads in chunks until it finds the
// terminator rather than guessing a maximum up front.
std::string readCString(domain::TargetSession& session, std::uintptr_t address) {
    constexpr std::size_t chunk = 64;
    constexpr std::size_t limit = 4096;

    std::string text;
    while (text.size() < limit) {
        auto bytes = session.readBytes(address + text.size(), chunk);
        if (!bytes || bytes.value().empty()) {
            return {};
        }
        for (const auto byte : bytes.value()) {
            if (byte == 0) {
                return text;
            }
            text.push_back(static_cast<char>(byte));
        }
        // A short read means the next page is not mapped, so there is no
        // terminator to find beyond it.
        if (bytes.value().size() < chunk) {
            return {};
        }
    }
    return {};
}

// Locates the export directory of the module at moduleBase. Returns false when
// the module has none, which is normal -- an EXE usually exports nothing.
bool findExportDirectory(domain::TargetSession& session, std::uintptr_t moduleBase, std::uint32_t& directoryRva,
                         std::uint32_t& directorySize) {
    auto headers = session.readBytes(moduleBase, 0x400);
    if (!headers || headers.value().size() < 0x40) {
        return false;
    }
    const auto& buffer = headers.value();

    std::uint16_t mz{};
    if (!readScalar(buffer, 0, mz) || mz != dosSignature) {
        return false;
    }

    std::uint32_t lfanew{};
    if (!readScalar(buffer, lfanewOffset, lfanew)) {
        return false;
    }

    std::uint32_t pe{};
    if (!readScalar(buffer, lfanew, pe) || pe != ntSignature) {
        return false;
    }

    const std::size_t optionalHeader = static_cast<std::size_t>(lfanew) + 4 + fileHeaderSize;
    std::uint16_t magic{};
    if (!readScalar(buffer, optionalHeader, magic)) {
        return false;
    }

    std::size_t directoryOffset{};
    if (magic == optionalMagic32) {
        directoryOffset = optionalHeader + dataDirectoryOffset32;
    } else if (magic == optionalMagic64) {
        directoryOffset = optionalHeader + dataDirectoryOffset64;
    } else {
        return false;
    }

    // Data directory entry 0 is the export table: an RVA then a size.
    if (!readScalar(buffer, directoryOffset, directoryRva) ||
        !readScalar(buffer, directoryOffset + 4, directorySize)) {
        return false;
    }
    return directoryRva != 0 && directorySize != 0;
}

} // namespace

std::uintptr_t ExportResolver::moduleBase(domain::TargetSession& session, const std::wstring& moduleName) {
    for (const auto& module : session.modules()) {
        if (sameModule(module.name, moduleName)) {
            return module.base;
        }
    }
    return 0;
}

Entries ExportResolver::exports(domain::TargetSession& session, std::uintptr_t moduleBase) const {
    if (!session.attached()) {
        return Entries::fail("No target process is attached.");
    }

    const auto generation = session.generation();
    {
        std::scoped_lock lock(cacheMutex_);
        if (generation != cacheGeneration_) {
            cache_.clear();
            cacheGeneration_ = generation;
        } else if (const auto cached = cache_.find(moduleBase); cached != cache_.end()) {
            return Entries::ok(cached->second);
        }
    }

    std::uint32_t directoryRva{};
    std::uint32_t directorySize{};
    if (!findExportDirectory(session, moduleBase, directoryRva, directorySize)) {
        return Entries::fail("The module at " + domain::toHex(moduleBase) + " has no readable export directory.");
    }

    // IMAGE_EXPORT_DIRECTORY is 40 bytes; the five fields we need start at 0x10.
    auto directory = session.readBytes(moduleBase + directoryRva, 40);
    if (!directory || directory.value().size() < 40) {
        return Entries::fail("Could not read the export directory.");
    }
    const auto& dir = directory.value();

    std::uint32_t ordinalBase{};
    std::uint32_t functionCount{};
    std::uint32_t nameCount{};
    std::uint32_t functionsRva{};
    std::uint32_t namesRva{};
    std::uint32_t ordinalsRva{};
    if (!readScalar(dir, 0x10, ordinalBase) || !readScalar(dir, 0x14, functionCount) ||
        !readScalar(dir, 0x18, nameCount) || !readScalar(dir, 0x1C, functionsRva) ||
        !readScalar(dir, 0x20, namesRva) || !readScalar(dir, 0x24, ordinalsRva)) {
        return Entries::fail("The export directory is truncated.");
    }

    // A module with more exports than this is not a module we were given; it is
    // a bad read being interpreted as counts. Refusing beats allocating on it.
    constexpr std::uint32_t sanityLimit = 100000;
    if (nameCount > sanityLimit || functionCount > sanityLimit) {
        return Entries::fail("The export directory is implausible and was rejected.");
    }
    if (nameCount == 0) {
        return Entries::ok({});
    }

    auto nameRvas = session.readBytes(moduleBase + namesRva, static_cast<std::size_t>(nameCount) * 4);
    auto ordinals = session.readBytes(moduleBase + ordinalsRva, static_cast<std::size_t>(nameCount) * 2);
    auto functions = session.readBytes(moduleBase + functionsRva, static_cast<std::size_t>(functionCount) * 4);
    if (!nameRvas || !ordinals || !functions || nameRvas.value().size() < static_cast<std::size_t>(nameCount) * 4 ||
        ordinals.value().size() < static_cast<std::size_t>(nameCount) * 2 ||
        functions.value().size() < static_cast<std::size_t>(functionCount) * 4) {
        return Entries::fail("The export tables could not be read in full.");
    }

    std::vector<ExportEntry> result;
    result.reserve(nameCount);

    for (std::uint32_t i = 0; i < nameCount; ++i) {
        std::uint32_t nameRva{};
        std::uint16_t ordinalIndex{};
        if (!readScalar(nameRvas.value(), static_cast<std::size_t>(i) * 4, nameRva) ||
            !readScalar(ordinals.value(), static_cast<std::size_t>(i) * 2, ordinalIndex)) {
            continue;
        }
        if (ordinalIndex >= functionCount) {
            continue;
        }

        std::uint32_t functionRva{};
        if (!readScalar(functions.value(), static_cast<std::size_t>(ordinalIndex) * 4, functionRva) ||
            functionRva == 0) {
            continue;
        }

        ExportEntry entry;
        entry.name = readCString(session, moduleBase + nameRva);
        if (entry.name.empty()) {
            continue;
        }
        entry.ordinal = static_cast<std::uint16_t>(ordinalIndex + ordinalBase);

        // An RVA landing inside the export directory itself is not code: it is a
        // "DLL.Function" string naming where the export really lives. This is
        // the single most common reason a hand-rolled resolver returns a
        // plausible-looking address that is not executable.
        if (functionRva >= directoryRva && functionRva < directoryRva + directorySize) {
            entry.unresolvedForwarder = readCString(session, moduleBase + functionRva);
        } else {
            entry.address = moduleBase + functionRva;
        }
        result.push_back(std::move(entry));
    }

    std::sort(result.begin(), result.end(),
              [](const ExportEntry& a, const ExportEntry& b) { return a.name < b.name; });

    {
        std::scoped_lock lock(cacheMutex_);
        // Only if the module table has not moved underneath this parse; a
        // stale entry is worse than no entry.
        if (session.generation() == generation) {
            cacheGeneration_ = generation;
            cache_[moduleBase] = result;
        }
    }
    return Entries::ok(std::move(result));
}

Address ExportResolver::resolve(domain::TargetSession& session, const std::wstring& moduleName,
                                const std::string& exportName) const {
    std::wstring currentModule = moduleName;
    std::string currentExport = exportName;

    for (int depth = 0; depth < maxForwarderDepth; ++depth) {
        const auto base = moduleBase(session, currentModule);
        if (base == 0) {
            return Address::fail(domain::narrow(currentModule) + " is not loaded in the target.");
        }

        auto table = exports(session, base);
        if (!table) {
            return Address::fail(table.error(), table.code());
        }

        const auto match = std::find_if(table.value().begin(), table.value().end(),
                                        [&currentExport](const ExportEntry& entry) {
                                            return entry.name == currentExport;
                                        });
        if (match == table.value().end()) {
            return Address::fail(currentExport + " is not exported by " + domain::narrow(currentModule) + ".");
        }

        if (match->unresolvedForwarder.empty()) {
            return Address::ok(match->address);
        }

        // "KERNELBASE.LoadLibraryW" -> module "KERNELBASE", export
        // "LoadLibraryW". A forwarder to an ordinal is written "DLL.#123"; we do
        // not follow those, because nothing we inject resolves by ordinal.
        const auto dot = match->unresolvedForwarder.rfind('.');
        if (dot == std::string::npos || dot + 1 >= match->unresolvedForwarder.size()) {
            return Address::fail(currentExport + " forwards to \"" + match->unresolvedForwarder +
                                 "\", which could not be parsed.");
        }
        const auto target = match->unresolvedForwarder.substr(dot + 1);
        if (target.front() == '#') {
            return Address::fail(currentExport + " forwards to ordinal " + target +
                                 ", which this resolver does not follow.");
        }

        currentModule = domain::widen(match->unresolvedForwarder.substr(0, dot));
        currentExport = target;
    }

    return Address::fail("The forwarder chain for " + exportName + " did not terminate.");
}

} // namespace ire::engine_symbols
