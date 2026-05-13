#include "engine_inject/Injector.h"

namespace ire::engine_inject {

Injector::Injector(domain::TargetSession& session) : session_(session) {}

infra::Result<std::uintptr_t> Injector::allocate(std::size_t size, DWORD protection) {
    if (!session_.attached()) {
        return infra::Result<std::uintptr_t>::fail("No target process attached.");
    }
    return session_.platform().allocate(session_.processHandle(), size, protection);
}

infra::Result<void> Injector::free(std::uintptr_t address) {
    if (!session_.attached()) {
        return infra::Result<void>::fail("No target process attached.");
    }
    return session_.platform().free(session_.processHandle(), address);
}

infra::Result<std::uint32_t> Injector::createThread(std::uintptr_t start, std::uintptr_t parameter) {
    if (!session_.attached()) {
        return infra::Result<std::uint32_t>::fail("No target process attached.");
    }
    return session_.platform().createRemoteThread(session_.processHandle(), start, parameter);
}

infra::Result<std::uint32_t> Injector::loadLibrary(const std::wstring& dllPath) {
    if (!session_.attached()) {
        return infra::Result<std::uint32_t>::fail("No target process attached.");
    }
    return session_.platform().injectLoadLibraryW(session_.processHandle(), dllPath);
}

} // namespace ire::engine_inject

