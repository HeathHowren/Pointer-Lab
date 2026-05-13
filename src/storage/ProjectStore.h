#pragma once

#include "domain/Domain.h"
#include "infra/Result.h"

#include <filesystem>
#include <vector>

namespace ire::storage {

struct ProjectTable {
    std::uint32_t lastPid{};
    std::wstring lastProcessName;
    std::vector<domain::AddressEntry> entries;
};

class ProjectStore {
public:
    infra::Result<void> save(const std::filesystem::path& path, const ProjectTable& table) const;
    infra::Result<ProjectTable> load(const std::filesystem::path& path) const;
};

} // namespace ire::storage

