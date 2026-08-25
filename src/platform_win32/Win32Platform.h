#pragma once

#include "domain/Domain.h"
#include "infra/Result.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ire::platform_win32 {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle();

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr);
    HANDLE release();

private:
    HANDLE handle_{nullptr};
};

class Win32Platform {
public:
    std::vector<domain::ProcessInfo> listProcesses() const;
    std::vector<domain::ModuleInfo> listModules(std::uint32_t pid) const;
    std::vector<domain::MemoryRegion> listMemoryRegions(HANDLE process) const;

    // limitedAccess, when supplied, reports that only a read-only handle could
    // be obtained so the caller can warn instead of letting every later write
    // fail one at a time with no explanation.
    infra::Result<UniqueHandle> openProcess(std::uint32_t pid, bool* limitedAccess = nullptr) const;

    // A short read is reported as success with a correspondingly shorter
    // buffer; callers that need an exact length must compare sizes. This keeps
    // scanning across partially-mapped regions working.
    infra::Result<std::vector<std::uint8_t>> readMemory(HANDLE process, std::uintptr_t address, std::size_t size) const;
    infra::Result<void> writeMemory(HANDLE process, std::uintptr_t address, const void* data, std::size_t size) const;
    infra::Result<void> protectMemory(HANDLE process, std::uintptr_t address, std::size_t size, DWORD protection, DWORD* oldProtection) const;
    infra::Result<std::uintptr_t> allocate(HANDLE process, std::size_t size, DWORD protection) const;
    infra::Result<void> free(HANDLE process, std::uintptr_t address) const;

    // Fails with a distinct message on timeout rather than reporting
    // STILL_ACTIVE (259) as though it were the thread's exit code.
    infra::Result<std::uint32_t> createRemoteThread(HANDLE process, std::uintptr_t start, std::uintptr_t parameter,
                                                    DWORD timeoutMs = 5000) const;
    infra::Result<std::uint32_t> injectLoadLibraryW(HANDLE process, const std::wstring& dllPath) const;

    // True when the target runs under WOW64 (a 32-bit process) and therefore
    // cannot be injected into from this 64-bit build.
    static bool isWow64(HANDLE process);
    // Requests SeDebugPrivilege; without it many processes cannot be opened.
    static infra::Result<void> enableDebugPrivilege();

    static bool isReadableProtect(DWORD protect);
    static bool isWritableProtect(DWORD protect);
    static bool isExecutableProtect(DWORD protect);
    static std::string formatLastError(DWORD error = GetLastError());
    static std::string protectToString(DWORD protect);
};

// Software and hardware breakpoints, done properly.
//
// Two things about this were badly wrong before. DebugActiveProcess ran on the
// UI thread while WaitForDebugEvent ran on another, and Windows requires both
// on the same thread, so no debug event was ever delivered. And a hit was
// resumed with DBG_CONTINUE without rewinding RIP or restoring the overwritten
// byte, so the target resumed one byte into an instruction and crashed.
//
// The pump therefore owns the whole lifecycle: it attaches, pumps and detaches
// on its own thread, and it owns the breakpoint table because arming has to be
// interleaved with the single-step dance that follows every hit.
//
// Hardware breakpoints share the table but none of that dance. They live in the
// CPU's four debug registers, which are per-thread, so the pump programs every
// thread the target has and every thread it goes on to create.
class DebugEventPump {
public:
    using HitCallback = std::function<void(const domain::BreakpointInfo&)>;
    using ExitCallback = std::function<void(std::uint32_t exitCode)>;

    DebugEventPump() = default;
    ~DebugEventPump();

    DebugEventPump(const DebugEventPump&) = delete;
    DebugEventPump& operator=(const DebugEventPump&) = delete;

    infra::Result<void> attach(std::uint32_t pid, HitCallback onHit, ExitCallback onExit);
    void detach();
    [[nodiscard]] bool attached() const { return attached_; }

    // A hardware breakpoint occupies one of the four debug registers and fails
    // with an explicit message once all four are taken -- that is a limit of the
    // CPU, not of Pointer Lab, so it is reported rather than worked around.
    // length is the number of bytes a data breakpoint watches (1, 2, 4 or 8) and
    // is ignored by the other kinds.
    infra::Result<void> addBreakpoint(std::uintptr_t address, std::string label,
                                      domain::BreakpointKind kind = domain::BreakpointKind::Software,
                                      std::uint8_t length = 1);
    infra::Result<void> removeBreakpoint(std::uintptr_t address);
    [[nodiscard]] std::vector<domain::BreakpointInfo> breakpoints() const;

private:
    void run(std::promise<infra::Result<void>> ready);
    void loop();
    bool handleBreakpoint(std::uintptr_t address, std::uint32_t threadId);
    bool handleSingleStep(std::uint32_t threadId);

    // What the debug registers say about a single-step exception.
    enum class HardwareVerdict {
        NotOurs, // No debug register fired; this is somebody else's exception.
        Stale,   // Ours, but its breakpoint is gone. Swallow without reporting.
        Hit      // A live hardware breakpoint fired.
    };

    // The two things a single-step exception can mean. Both expect mutex_ to be
    // held. handleSoftwareStep returns whether the exception was ours.
    HardwareVerdict handleHardwareHit(std::uint32_t threadId, domain::BreakpointInfo& snapshot);
    bool handleSoftwareStep(std::map<std::uint32_t, std::uintptr_t>::iterator stepping);
    bool rewindThread(std::uintptr_t address, std::uint32_t threadId) const;
    void disarmAll();
    void drainPendingEvents();

    // Debug-register plumbing. All four expect mutex_ to be held: breakpoints_
    // is the authority for what the registers should contain, and a thread that
    // reads it while an add is half-applied programs the wrong thing.
    //
    // The registers are always written wholesale from the table rather than
    // patched a bit at a time, so a thread created during an add cannot end up
    // disagreeing with the others about DR7.
    infra::Result<void> addHardwareBreakpoint(std::uintptr_t address, std::string label, domain::BreakpointKind kind,
                                              std::uint8_t length);
    [[nodiscard]] bool anyHardwareArmed() const;
    [[nodiscard]] int freeDebugSlot() const;
    bool writeDebugRegisters(HANDLE thread) const;
    void applyDebugRegistersToAllThreads() const;

    [[nodiscard]] infra::Result<std::uint8_t> readByte(std::uintptr_t address) const;
    infra::Result<void> writeByte(std::uintptr_t address, std::uint8_t value) const;

    Win32Platform platform_;
    std::uint32_t pid_{};
    UniqueHandle process_;
    std::atomic<bool> attached_{false};
    std::atomic<bool> stop_{false};
    // Set when the target itself exited, so we do not try to detach from a
    // process that is already gone.
    bool targetExited_{};
    HitCallback onHit_;
    ExitCallback onExit_;
    std::jthread worker_;

    mutable std::mutex mutex_;
    std::map<std::uintptr_t, domain::BreakpointInfo> breakpoints_;
    // Threads currently stepping over a restored breakpoint, and which one.
    std::map<std::uint32_t, std::uintptr_t> stepping_;
    // Addresses whose original byte has been put back but whose int3 may still
    // be in flight. A thread can execute the trap microseconds before the byte
    // is restored, and its exception arrives afterwards; treating that as
    // somebody else's breakpoint passes it through and kills the target.
    std::set<std::uintptr_t> disarmed_;
    // Whether any hardware breakpoint has been set during this attach. The
    // hardware equivalent of disarmed_: a debug-register trap already in flight
    // when its register was cleared arrives with DR6 wiped and nothing left to
    // match it against, so after hardware has been used once an unclaimed
    // single-step exception has to be treated as ours.
    bool hardwareUsed_{};
};

} // namespace ire::platform_win32

