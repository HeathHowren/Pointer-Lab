#pragma once

#include "domain/Domain.h"
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
    std::vector<domain::AddressEntry> entries;
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

