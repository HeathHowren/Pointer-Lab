#pragma once

#include "domain/TargetSession.h"

namespace ire::engine_inject {

// Allocates, runs and loads code inside the attached process.
//
// Every one of these can crash or destabilise the target if pointed at the
// wrong address, so the UI puts each behind its own confirmation dialog. None
// of them is undone automatically: memory allocated here stays allocated, and a
// library loaded here stays loaded, until the target exits.
class Injector {
public:
    explicit Injector(domain::TargetSession& session);

    // Reserves and commits size bytes in the target. protection is a Win32
    // PAGE_* constant. Returns the base address in the target's address space.
    infra::Result<std::uintptr_t> allocate(std::size_t size, DWORD protection);

    // Allocates within 2 GB of hint, so a five-byte `jmp` from there can reach
    // it. Fails rather than allocating out of reach; see Win32Platform.
    infra::Result<std::uintptr_t> allocateNear(std::size_t size, DWORD protection, std::uintptr_t hint);

    // Releases a block previously returned by allocate(). Freeing memory that
    // the target itself is still using will crash it.
    infra::Result<void> free(std::uintptr_t address);

    // Starts a thread in the target at start, passing parameter as its single
    // argument, and waits up to five seconds for it. Returns the thread's exit
    // code -- not its id, which is discarded. A thread still running when the
    // wait expires fails with a message saying so rather than reporting
    // STILL_ACTIVE as though it were an exit code; the thread keeps running.
    infra::Result<std::uint32_t> createThread(std::uintptr_t start, std::uintptr_t parameter);

    // Loads a DLL into the target by calling its LoadLibraryW on a new thread,
    // and waits for that thread to finish. Returns the thread's exit code,
    // which is the low 32 bits of the returned module handle. The path is
    // written into the target and freed only after the wait completes.
    infra::Result<std::uint32_t> loadLibrary(const std::wstring& dllPath);

private:
    domain::TargetSession& session_;
};

} // namespace ire::engine_inject

