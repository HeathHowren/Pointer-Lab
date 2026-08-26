#pragma once

#include "domain/Domain.h"
#include "domain/Structure.h"
#include "infra/Result.h"

#include <filesystem>
#include <vector>

namespace ire::storage {

struct ProjectTable {
    // Advisory only. The pid is recorded so the process list can preselect a
    // likely target, never to reattach automatically — by the next run it
    // almost certainly belongs to something else.
    std::uint32_t lastPid{};
    std::wstring lastProcessName;
    // Width of the process this table was built against. Pointer chains are
    // stored as module+offset and so survive a restart, but they do not survive
    // being pointed at a target of the other bitness -- the offsets were
    // measured against a different struct layout. Recorded so loading a
    // mismatched table can say so instead of resolving to nonsense.
    domain::Bitness lastBitness{domain::Bitness::X64};
    std::vector<domain::AddressEntry> entries;
    // User-defined symbols, stored as the expression that produced them rather
    // than as the address it produced. An address from a previous run names
    // nothing after ASLR moves the module; `client.dll+0x4A2C10` still does.
    struct SavedSymbol {
        std::string name;
        std::string expression;
    };
    std::vector<SavedSymbol> symbols;
    // Auto-assembler scripts, saved as text and always loaded switched off. The
    // enabled flag deliberately does not persist: at load time there is no
    // target, nothing has been patched, and a script that claimed to be on would
    // offer an undo for changes that were never made.
    struct SavedScript {
        std::string name;
        std::string source;
    };
    std::vector<SavedScript> scripts;
    // Structure definitions. Saved without the addresses they were last laid
    // over: a layout is knowledge about the game and outlives every run, while
    // the address of one particular enemy does not survive the next respawn.
    std::vector<domain::Structure> structures;
};

// Reads and writes `.iretable` project files. The format is specified in
// docs/iretable-format.md.
class ProjectStore {
public:
    // Writes the current format version (3). Fails if the file cannot be opened
    // or the write does not flush, so a full or read-only disk is reported
    // rather than assumed to have worked.
    infra::Result<void> save(const std::filesystem::path& path, const ProjectTable& table) const;

    // Reads versions 1, 2 and 3. Fails only on an unreadable file or an
    // unrecognised header; a corrupt row costs that row, and a corrupt pointer
    // chain costs only the chain, leaving the entry as a fixed address. Both
    // are logged with the line number.
    infra::Result<ProjectTable> load(const std::filesystem::path& path) const;
};

} // namespace ire::storage

