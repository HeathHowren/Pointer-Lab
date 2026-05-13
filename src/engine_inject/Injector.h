#pragma once

#include "domain/TargetSession.h"

namespace ire::engine_inject {

class Injector {
public:
    explicit Injector(domain::TargetSession& session);

    infra::Result<std::uintptr_t> allocate(std::size_t size, DWORD protection);
    infra::Result<void> free(std::uintptr_t address);
    infra::Result<std::uint32_t> createThread(std::uintptr_t start, std::uintptr_t parameter);
    infra::Result<std::uint32_t> loadLibrary(const std::wstring& dllPath);

private:
    domain::TargetSession& session_;
};

} // namespace ire::engine_inject

