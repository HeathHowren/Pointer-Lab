#pragma once

#include "domain/TargetSession.h"
#include "infra/Result.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ire::engine_symbols {

struct ExportEntry {
    std::string name;
    // Absolute address in the target, not an RVA.
    std::uintptr_t address{};
    std::uint16_t ordinal{};
    // Set when this export forwards elsewhere and the forwarder could not be
    // followed. address is 0 in that case.
    std::string unresolvedForwarder;
};

// Reads a module's export directory out of the *target's* memory.
//
// This exists because GetProcAddress answers about Pointer Lab's own address
// space, and for injection we need an address valid in the target's. The two
// differ for two separate reasons, and either one alone is enough to break it:
//
//   - A 32-bit (WOW64) target loads an entirely different kernel32 from the one
//     this 64-bit process has mapped. Passing our LoadLibraryW to it sends a
//     remote thread to an address that is not code in that process.
//   - Even for a same-bitness target, nothing guarantees a system DLL is mapped
//     at the same base in both processes.
//
// Parsing the headers ourselves also means no dependency on the symbol server,
// no PDBs and no DbgHelp: an export directory is in every PE file by
// construction, which is exactly why injection has always used exports.
class ExportResolver {
public:
    // Resolves one export by name, following forwarder chains (kernel32's
    // LoadLibraryW is a forwarder to KERNELBASE on every modern Windows, so a
    // resolver that ignores forwarders returns a string address and the remote
    // thread jumps into text).
    infra::Result<std::uintptr_t> resolve(domain::TargetSession& session, const std::wstring& moduleName,
                                          const std::string& exportName) const;

    // Every named export of the module loaded at moduleBase, in name order.
    infra::Result<std::vector<ExportEntry>> exports(domain::TargetSession& session, std::uintptr_t moduleBase) const;

    // Looks a loaded module up by name, case-insensitively and tolerating a
    // missing ".dll". Returns 0 when it is not loaded in the target.
    [[nodiscard]] static std::uintptr_t moduleBase(domain::TargetSession& session, const std::wstring& moduleName);

private:
    // Parsing one export directory costs a read per named export -- around
    // fifteen hundred of them for kernel32. That is affordable once and
    // ruinous per frame, and an address box that names an export resolves on
    // every frame it is on screen. The cache is dropped whenever the module
    // table changes, which is the only time a module's exports can move.
    mutable std::mutex cacheMutex_;
    mutable std::uint64_t cacheGeneration_{};
    mutable std::map<std::uintptr_t, std::vector<ExportEntry>> cache_;
};

} // namespace ire::engine_symbols
