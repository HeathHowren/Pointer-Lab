#include "engine_inject/Injector.h"

#include "engine_symbols/ExportResolver.h"
#include "infra/Logger.h"

namespace ire::engine_inject {

Injector::Injector(domain::TargetSession& session) : session_(session) {}

infra::Result<std::uintptr_t> Injector::allocate(std::size_t size, DWORD protection) {
    if (!session_.attached()) {
        return infra::Result<std::uintptr_t>::fail("No target process attached.");
    }
    return session_.platform().allocate(session_.processHandle(), size, protection);
}

infra::Result<std::uintptr_t> Injector::allocateNear(std::size_t size, DWORD protection, std::uintptr_t hint) {
    if (!session_.attached()) {
        return infra::Result<std::uintptr_t>::fail("No target process attached.");
    }
    return session_.platform().allocateNear(session_.processHandle(), size, protection, hint);
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

    // Resolve LoadLibraryW out of the target's own kernel32 rather than ours.
    // For a 32-bit target the two are different DLLs entirely; even for a
    // 64-bit one the bases can differ. Resolving here is also what makes this
    // work at all under WOW64, which used to be refused outright.
    //
    // A failure is not fatal: for a same-bitness target the platform layer can
    // still fall back to this process's address, which is what shipped before.
    std::uintptr_t loadLibrary{};
    const engine_symbols::ExportResolver resolver;
    if (auto resolved = resolver.resolve(session_, L"kernel32.dll", "LoadLibraryW")) {
        loadLibrary = resolved.value();
    } else {
        infra::Logger::instance().warn("Could not resolve LoadLibraryW in the target (" + resolved.error() +
                                       "). Falling back to this process's own address.");
    }

    return session_.platform().injectLoadLibraryW(session_.processHandle(), dllPath, loadLibrary);
}

} // namespace ire::engine_inject

