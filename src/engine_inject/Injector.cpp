#include "engine_inject/Injector.h"

#include "engine_symbols/ExportResolver.h"
#include "infra/Logger.h"

namespace ire::engine_inject {

Injector::Injector(domain::TargetSession& session) : session_(session) {}

// Every operation here takes its own duplicate of the process handle for as
// long as it runs. The session's handle is closed under the session's lock on
// detach, and a raw copy of it used after that lock was released -- which is
// what these did -- could be dead, or worse, reused for something else by
// the time createRemoteThread had waited its five seconds.

infra::Result<std::uintptr_t> Injector::allocate(std::size_t size, DWORD protection) {
    auto process = session_.duplicateHandle();
    if (!process) {
        return infra::Result<std::uintptr_t>::fail(process.error(), process.code());
    }
    return session_.platform().allocate(process.value().get(), size, protection);
}

infra::Result<std::uintptr_t> Injector::allocateNear(std::size_t size, DWORD protection, std::uintptr_t hint) {
    auto process = session_.duplicateHandle();
    if (!process) {
        return infra::Result<std::uintptr_t>::fail(process.error(), process.code());
    }
    return session_.platform().allocateNear(process.value().get(), size, protection, hint);
}

infra::Result<void> Injector::free(std::uintptr_t address) {
    auto process = session_.duplicateHandle();
    if (!process) {
        return infra::Result<void>::fail(process.error(), process.code());
    }
    return session_.platform().free(process.value().get(), address);
}

infra::Result<std::uint32_t> Injector::createThread(std::uintptr_t start, std::uintptr_t parameter) {
    auto process = session_.duplicateHandle();
    if (!process) {
        return infra::Result<std::uint32_t>::fail(process.error(), process.code());
    }
    return session_.platform().createRemoteThread(process.value().get(), start, parameter);
}

infra::Result<std::uint32_t> Injector::loadLibrary(const std::wstring& dllPath) {
    auto process = session_.duplicateHandle();
    if (!process) {
        return infra::Result<std::uint32_t>::fail(process.error(), process.code());
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

    return session_.platform().injectLoadLibraryW(process.value().get(), dllPath, loadLibrary);
}

} // namespace ire::engine_inject

