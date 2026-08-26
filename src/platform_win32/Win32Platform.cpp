#include "platform_win32/Win32Platform.h"

#include "infra/Logger.h"

#include <TlHelp32.h>
#include <processthreadsapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace ire::platform_win32 {

UniqueHandle::~UniqueHandle() {
    reset();
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void UniqueHandle::reset(HANDLE handle) {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
    }
    handle_ = handle;
}

HANDLE UniqueHandle::release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
}

std::vector<domain::ProcessInfo> Win32Platform::listProcesses() const {
    std::vector<domain::ProcessInfo> processes;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return processes;
    }

    do {
        processes.push_back({entry.th32ProcessID, entry.szExeFile});
    } while (Process32NextW(snapshot.get(), &entry));

    std::sort(processes.begin(), processes.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return processes;
}

std::vector<domain::ModuleInfo> Win32Platform::listModules(std::uint32_t pid) const {
    std::vector<domain::ModuleInfo> modules;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) {
        return modules;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return modules;
    }

    do {
        modules.push_back({
            reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
            static_cast<std::size_t>(entry.modBaseSize),
            entry.szModule,
            entry.szExePath
        });
    } while (Module32NextW(snapshot.get(), &entry));

    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.base < b.base; });
    return modules;
}

std::vector<domain::MemoryRegion> Win32Platform::listMemoryRegions(HANDLE process) const {
    std::vector<domain::MemoryRegion> regions;
    if (!process) {
        return regions;
    }

    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    auto address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T queried = VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi));
        if (queried == 0) {
            address += 0x10000;
            continue;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto size = static_cast<std::size_t>(mbi.RegionSize);
        regions.push_back({
            base,
            size,
            mbi.State,
            mbi.Protect,
            mbi.Type,
            mbi.State == MEM_COMMIT && isReadableProtect(mbi.Protect),
            mbi.State == MEM_COMMIT && isWritableProtect(mbi.Protect),
            mbi.State == MEM_COMMIT && isExecutableProtect(mbi.Protect)
        });

        const auto next = base + size;
        if (next <= address) {
            break;
        }
        address = next;
    }

    return regions;
}

infra::Result<UniqueHandle> Win32Platform::openProcess(std::uint32_t pid, bool* limitedAccess) const {
    if (limitedAccess) {
        *limitedAccess = false;
    }

    const DWORD rights =
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
        PROCESS_CREATE_THREAD | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;

    HANDLE process = OpenProcess(rights, FALSE, pid);
    if (!process) {
        // Remember why full access failed; the fallback overwrites GetLastError.
        const DWORD fullAccessError = GetLastError();
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process) {
            return infra::Result<UniqueHandle>::fail(formatLastError(fullAccessError), fullAccessError);
        }
        if (limitedAccess) {
            *limitedAccess = true;
        }
    }
    return infra::Result<UniqueHandle>::ok(UniqueHandle(process));
}

bool Win32Platform::isWow64(HANDLE process) {
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64)) {
        return false;
    }
    return wow64 != FALSE;
}

infra::Result<void> Win32Platform::enableDebugPrivilege() {
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
        return infra::Result<void>::fail(formatLastError(), GetLastError());
    }
    UniqueHandle token(rawToken);

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        return infra::Result<void>::fail(formatLastError(), GetLastError());
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        return infra::Result<void>::fail(formatLastError(), GetLastError());
    }
    // AdjustTokenPrivileges reports success even when the privilege was not
    // held, so the real outcome is only visible in GetLastError.
    const DWORD status = GetLastError();
    if (status == ERROR_NOT_ALL_ASSIGNED) {
        return infra::Result<void>::fail("SeDebugPrivilege is not held; run as administrator for full process access.", status);
    }
    return infra::Result<void>::ok();
}

infra::Result<std::vector<std::uint8_t>> Win32Platform::readMemory(HANDLE process, std::uintptr_t address, std::size_t size) const {
    std::vector<std::uint8_t> bytes(size);
    SIZE_T read{};
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(), size, &read)) {
        const DWORD error = GetLastError();
        if (read == 0) {
            return infra::Result<std::vector<std::uint8_t>>::fail(formatLastError(error), error);
        }
        // Partial read (typically ERROR_PARTIAL_COPY at the end of a mapped
        // range). The shortened buffer below is the caller's signal.
    }
    bytes.resize(read);
    return infra::Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

infra::Result<void> Win32Platform::writeMemory(HANDLE process, std::uintptr_t address, const void* data, std::size_t size) const {
    if (size == 0) {
        return infra::Result<void>::ok();
    }

    auto* target = reinterpret_cast<LPVOID>(address);

    // Try the write as-is first. Most writes land on pages that are already
    // writable, and forcing PAGE_EXECUTE_READWRITE on every write needlessly
    // made data pages executable and left a very visible footprint.
    SIZE_T written{};
    if (WriteProcessMemory(process, target, data, size, &written) && written == size) {
        FlushInstructionCache(process, target, size);
        return infra::Result<void>::ok();
    }

    // Escalate: make the range writable, write, then put the protection back.
    DWORD oldProtection{};
    if (!VirtualProtectEx(process, target, size, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        const DWORD error = GetLastError();
        return infra::Result<void>::fail("Could not make the target memory writable: " + formatLastError(error), error);
    }

    written = 0;
    const BOOL wrote = WriteProcessMemory(process, target, data, size, &written);
    const DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
    FlushInstructionCache(process, target, size);

    // Always restore, even when the write failed.
    DWORD restored{};
    if (!VirtualProtectEx(process, target, size, oldProtection, &restored)) {
        infra::Logger::instance().warn("Could not restore memory protection at " + domain::toHex(address) + ".");
    }

    if (!wrote || written != size) {
        return infra::Result<void>::fail(formatLastError(writeError), writeError);
    }
    return infra::Result<void>::ok();
}

infra::Result<void> Win32Platform::protectMemory(HANDLE process, std::uintptr_t address, std::size_t size, DWORD protection, DWORD* oldProtection) const {
    if (!VirtualProtectEx(process, reinterpret_cast<LPVOID>(address), size, protection, oldProtection)) {
        return infra::Result<void>::fail(formatLastError());
    }
    return infra::Result<void>::ok();
}

infra::Result<std::uintptr_t> Win32Platform::allocate(HANDLE process, std::size_t size, DWORD protection) const {
    auto* memory = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, protection);
    if (!memory) {
        return infra::Result<std::uintptr_t>::fail(formatLastError());
    }
    return infra::Result<std::uintptr_t>::ok(reinterpret_cast<std::uintptr_t>(memory));
}

infra::Result<std::uintptr_t> Win32Platform::allocateNear(HANDLE process, std::size_t size, DWORD protection,
                                                          std::uintptr_t hint) const {
    using Address = infra::Result<std::uintptr_t>;
    if (hint == 0) {
        return allocate(process, size, protection);
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t granularity = info.dwAllocationGranularity != 0 ? info.dwAllocationGranularity : 0x10000;

    // Just under 2 GB. The displacement is measured from the *end* of the jump
    // and the block has to fit entirely within reach, so the margin is not
    // pedantry -- a cave placed at exactly 2 GB is one byte too far.
    constexpr std::uintptr_t reach = 0x7FF00000;
    const std::uintptr_t low = hint > reach ? hint - reach : 0;
    const std::uintptr_t high = hint > UINTPTR_MAX - reach ? UINTPTR_MAX : hint + reach;

    const auto alignUp = [granularity](std::uintptr_t value) {
        return (value + granularity - 1) & ~(granularity - 1);
    };

    // Walks free regions with VirtualQueryEx rather than probing every 64 KB
    // slot: the search range holds 32,768 of them, and asking the kernel where
    // the holes are costs one call per region instead of one per slot.
    const auto search = [&](std::uintptr_t from, std::uintptr_t to) -> std::uintptr_t {
        MEMORY_BASIC_INFORMATION region{};
        for (std::uintptr_t address = alignUp(from); address < to;) {
            if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) !=
                sizeof(region)) {
                break;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            const auto end = base + region.RegionSize;
            if (region.State == MEM_FREE) {
                for (auto candidate = alignUp(std::max(base, from));
                     candidate + size <= end && candidate < to; candidate += granularity) {
                    if (auto* memory = VirtualAllocEx(process, reinterpret_cast<LPVOID>(candidate), size,
                                                      MEM_COMMIT | MEM_RESERVE, protection)) {
                        return reinterpret_cast<std::uintptr_t>(memory);
                    }
                }
            }
            if (end <= address) {
                break; // no forward progress; refuse to spin
            }
            address = end;
        }
        return 0;
    };

    // Above the hint first, then below. Either satisfies the jump; starting
    // above keeps the walk in one direction, which is the direction
    // VirtualQueryEx enumerates anyway.
    if (const auto above = search(hint, high); above != 0) {
        return Address::ok(above);
    }
    if (const auto below = search(low, hint); below != 0) {
        return Address::ok(below);
    }
    return Address::fail("No free memory within 2 GB of " + domain::toHex(hint) +
                         ", so a five-byte jump could not reach the allocation.");
}

infra::Result<void> Win32Platform::free(HANDLE process, std::uintptr_t address) const {
    if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(address), 0, MEM_RELEASE)) {
        return infra::Result<void>::fail(formatLastError());
    }
    return infra::Result<void>::ok();
}

infra::Result<std::uint32_t> Win32Platform::createRemoteThread(HANDLE process, std::uintptr_t start, std::uintptr_t parameter,
                                                              DWORD timeoutMs) const {
    UniqueHandle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), reinterpret_cast<LPVOID>(parameter), 0, nullptr));
    if (!thread) {
        return infra::Result<std::uint32_t>::fail(formatLastError(), GetLastError());
    }

    const DWORD waited = WaitForSingleObject(thread.get(), timeoutMs);
    if (waited == WAIT_TIMEOUT) {
        // Reporting GetExitCodeThread's STILL_ACTIVE (259) here used to look
        // exactly like a thread that had finished and returned 259.
        return infra::Result<std::uint32_t>::fail(
            "The remote thread did not finish within " + std::to_string(timeoutMs) + " ms and is still running.",
            WAIT_TIMEOUT);
    }
    if (waited != WAIT_OBJECT_0) {
        const DWORD error = GetLastError();
        return infra::Result<std::uint32_t>::fail("Waiting on the remote thread failed: " + formatLastError(error), error);
    }

    DWORD exitCode{};
    if (!GetExitCodeThread(thread.get(), &exitCode)) {
        const DWORD error = GetLastError();
        return infra::Result<std::uint32_t>::fail(formatLastError(error), error);
    }
    return infra::Result<std::uint32_t>::ok(exitCode);
}

infra::Result<std::uint32_t> Win32Platform::injectLoadLibraryW(HANDLE process, const std::wstring& dllPath,
                                                               std::uintptr_t loadLibraryAddress) const {
    if (dllPath.empty()) {
        return infra::Result<std::uint32_t>::fail("No DLL path was given.");
    }

    std::uintptr_t loadLibrary = loadLibraryAddress;
    if (loadLibrary == 0) {
        // Falling back to our own kernel32 is only correct for a target of the
        // same architecture and the same kernel32 base. The caller is expected
        // to have resolved the address out of the target for anything else --
        // this path exists so a same-bitness inject still works if that fails.
        if (isWow64(process)) {
            return infra::Result<std::uint32_t>::fail(
                "The target is a 32-bit process and its LoadLibraryW address could not be resolved. "
                "Injecting this process's own address would send the remote thread into unmapped memory.");
        }

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) {
            return infra::Result<std::uint32_t>::fail("Could not locate kernel32.dll.", GetLastError());
        }
        loadLibrary = reinterpret_cast<std::uintptr_t>(GetProcAddress(kernel32, "LoadLibraryW"));
        if (loadLibrary == 0) {
            return infra::Result<std::uint32_t>::fail("Could not resolve LoadLibraryW.", GetLastError());
        }
    }

    const auto bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    auto remote = allocate(process, bytes, PAGE_READWRITE);
    if (!remote) {
        return infra::Result<std::uint32_t>::fail(remote.error(), remote.code());
    }

    auto write = writeMemory(process, remote.value(), dllPath.c_str(), bytes);
    if (!write) {
        auto released = free(process, remote.value());
        if (!released) {
            infra::Logger::instance().warn("Could not release the remote path buffer: " + released.error());
        }
        return infra::Result<std::uint32_t>::fail(write.error(), write.code());
    }

    auto result = createRemoteThread(process, loadLibrary, remote.value());

    // Only release the path buffer once the remote thread has actually
    // finished with it. Freeing it after a timeout would pull the string out
    // from under a LoadLibraryW call that is still reading it, crashing the
    // target; leaking one small allocation is the far better outcome.
    if (result) {
        auto released = free(process, remote.value());
        if (!released) {
            infra::Logger::instance().warn("Could not release the remote path buffer: " + released.error());
        }
    } else {
        infra::Logger::instance().warn(
            "Leaving the remote path buffer at " + domain::toHex(remote.value()) +
            " allocated because the injection thread did not complete; freeing it could crash the target.");
    }
    return result;
}

bool Win32Platform::isReadableProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }
    const DWORD base = protect & 0xff;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
        base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool Win32Platform::isWritableProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }
    const DWORD base = protect & 0xff;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool Win32Platform::isExecutableProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }
    const DWORD base = protect & 0xff;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

std::string Win32Platform::formatLastError(DWORD error) {
    LPWSTR message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring wide = message ? message : L"Unknown error";
    if (message) {
        LocalFree(message);
    }
    return domain::narrow(wide);
}

std::string Win32Platform::protectToString(DWORD protect) {
    if (protect & PAGE_GUARD) {
        return "GUARD";
    }
    switch (protect & 0xff) {
    case PAGE_NOACCESS: return "NOACCESS";
    case PAGE_READONLY: return "R";
    case PAGE_READWRITE: return "RW";
    case PAGE_WRITECOPY: return "WC";
    case PAGE_EXECUTE: return "X";
    case PAGE_EXECUTE_READ: return "XR";
    case PAGE_EXECUTE_READWRITE: return "XRW";
    case PAGE_EXECUTE_WRITECOPY: return "XWC";
    default: return "?";
    }
}

namespace {

constexpr std::uint8_t int3 = 0xCC;
// EFLAGS.TF. Set it and the CPU raises a single-step exception after exactly
// one instruction, which is how a breakpoint gets re-armed behind itself.
constexpr DWORD trapFlag = 0x100;
// EFLAGS.RF. An execute breakpoint faults *before* the instruction runs, so
// resuming without this re-faults on the same instruction forever. RF suppresses
// instruction breakpoints for exactly one instruction and the CPU clears it
// again afterwards.
constexpr DWORD resumeFlag = 0x10000;
// There are four of them, and that is a property of the CPU.
constexpr int debugSlotCount = 4;
// DR6 bits 0-3: which debug register caused this exception.
constexpr DWORD64 debugStatusMask = 0xF;

// DR7 R/W encoding for a slot: what kind of access the CPU should trap.
// 10 is I/O access, which needs DE in CR4 and is no use here.
DWORD64 readWriteBits(domain::BreakpointKind kind) {
    switch (kind) {
    case domain::BreakpointKind::HardwareWrite: return 0b01;
    case domain::BreakpointKind::HardwareReadWrite: return 0b11;
    case domain::BreakpointKind::HardwareExecute:
    case domain::BreakpointKind::Software: break;
    }
    return 0b00;
}

// DR7 LEN encoding. Note that 8 bytes is 0b10, out of numeric order.
DWORD64 lengthBits(std::uint8_t length) {
    switch (length) {
    case 2: return 0b01;
    case 8: return 0b10;
    case 4: return 0b11;
    default: return 0b00;
    }
}

// Enumerates the target's threads. The debug registers are per-thread, so every
// one of them has to be programmed for a breakpoint to be reliable.
std::vector<std::uint32_t> threadIdsOf(std::uint32_t pid) {
    std::vector<std::uint32_t> ids;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) {
        return ids;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.get(), &entry)) {
        return ids;
    }
    do {
        if (entry.th32OwnerProcessID == pid) {
            ids.push_back(entry.th32ThreadID);
        }
    } while (Thread32Next(snapshot.get(), &entry));
    return ids;
}

// How long detach() keeps pumping to let threads finish a step already in
// progress, rather than abandoning them with the trap flag still set.
constexpr auto drainTimeout = std::chrono::seconds(2);

// One thread's register state, in whichever of the two shapes the target needs.
//
// A WOW64 thread has two contexts. The 64-bit one, which GetThreadContext
// returns, describes the thunk layer in wow64cpu.dll -- its Rip is inside
// Windows' own emulation code, not inside the target's. The 32-bit one, reached
// through Wow64GetThreadContext, is the code the target actually wrote.
//
// Every value this pump cares about lives in the second: the instruction
// pointer to rewind onto a breakpoint, the trap flag to single-step with, and
// the debug registers to program. Using the 64-bit context against a 32-bit
// target does not fail loudly -- it reads and writes a real, valid context that
// simply belongs to the wrong layer, so breakpoints never fire and rewinding
// corrupts the emulator.
class ThreadContext {
public:
    enum Parts : unsigned {
        Control = 1u << 0,
        Integer = 1u << 1,
        DebugRegisters = 1u << 2
    };

    explicit ThreadContext(bool wow64) : wow64_(wow64) {}

    bool get(HANDLE thread, unsigned parts) {
        if (wow64_) {
            wow_ = WOW64_CONTEXT{};
            wow_.ContextFlags = wowFlags(parts);
            return Wow64GetThreadContext(thread, &wow_) != FALSE;
        }
        native_ = CONTEXT{};
        native_.ContextFlags = nativeFlags(parts);
        return GetThreadContext(thread, &native_) != FALSE;
    }

    bool set(HANDLE thread, unsigned parts) {
        if (wow64_) {
            wow_.ContextFlags = wowFlags(parts);
            return Wow64SetThreadContext(thread, &wow_) != FALSE;
        }
        native_.ContextFlags = nativeFlags(parts);
        return SetThreadContext(thread, &native_) != FALSE;
    }

    [[nodiscard]] std::uint64_t instructionPointer() const {
        return wow64_ ? static_cast<std::uint64_t>(wow_.Eip) : native_.Rip;
    }

    void setInstructionPointer(std::uint64_t value) {
        if (wow64_) {
            wow_.Eip = static_cast<DWORD>(value);
        } else {
            native_.Rip = value;
        }
    }

    void orFlags(std::uint32_t bits) {
        if (wow64_) {
            wow_.EFlags |= bits;
        } else {
            native_.EFlags |= bits;
        }
    }

    [[nodiscard]] DWORD64 debugStatus() const {
        return wow64_ ? static_cast<DWORD64>(wow_.Dr6) : native_.Dr6;
    }

    void clearDebugStatus() {
        if (wow64_) {
            wow_.Dr6 = 0;
        } else {
            native_.Dr6 = 0;
        }
    }

    void programDebugRegisters(const std::array<DWORD64, debugSlotCount>& addresses, DWORD64 control) {
        if (wow64_) {
            wow_.Dr0 = static_cast<DWORD>(addresses[0]);
            wow_.Dr1 = static_cast<DWORD>(addresses[1]);
            wow_.Dr2 = static_cast<DWORD>(addresses[2]);
            wow_.Dr3 = static_cast<DWORD>(addresses[3]);
            wow_.Dr6 = 0;
            wow_.Dr7 = static_cast<DWORD>(control);
        } else {
            native_.Dr0 = addresses[0];
            native_.Dr1 = addresses[1];
            native_.Dr2 = addresses[2];
            native_.Dr3 = addresses[3];
            native_.Dr6 = 0;
            native_.Dr7 = control;
        }
    }

    void capture(std::uint32_t threadId, domain::RegisterContext& out) const {
        if (wow64_) {
            // r8-r15 stay zero: a 32-bit thread has no such registers, and the
            // UI hides them rather than printing eight zeroes.
            out = domain::RegisterContext{};
            out.rip = wow_.Eip;
            out.rsp = wow_.Esp;
            out.rbp = wow_.Ebp;
            out.rax = wow_.Eax;
            out.rbx = wow_.Ebx;
            out.rcx = wow_.Ecx;
            out.rdx = wow_.Edx;
            out.rsi = wow_.Esi;
            out.rdi = wow_.Edi;
            out.eflags = wow_.EFlags;
            out.bitness = domain::Bitness::X86;
        } else {
            out.rip = native_.Rip;
            out.rsp = native_.Rsp;
            out.rbp = native_.Rbp;
            out.rax = native_.Rax;
            out.rbx = native_.Rbx;
            out.rcx = native_.Rcx;
            out.rdx = native_.Rdx;
            out.rsi = native_.Rsi;
            out.rdi = native_.Rdi;
            out.r8 = native_.R8;
            out.r9 = native_.R9;
            out.r10 = native_.R10;
            out.r11 = native_.R11;
            out.r12 = native_.R12;
            out.r13 = native_.R13;
            out.r14 = native_.R14;
            out.r15 = native_.R15;
            out.eflags = native_.EFlags;
            out.bitness = domain::Bitness::X64;
        }
        out.threadId = threadId;
        out.captured = true;
    }

private:
    // The two vocabularies do not share values -- CONTEXT_CONTROL is 0x00100001
    // and WOW64_CONTEXT_CONTROL is 0x00010001 -- so callers use the Parts enum
    // above and never touch either constant directly.
    static DWORD nativeFlags(unsigned parts) {
        DWORD flags = 0;
        if (parts & Control) flags |= CONTEXT_CONTROL;
        if (parts & Integer) flags |= CONTEXT_INTEGER;
        if (parts & DebugRegisters) flags |= CONTEXT_DEBUG_REGISTERS;
        return flags;
    }

    static DWORD wowFlags(unsigned parts) {
        DWORD flags = 0;
        if (parts & Control) flags |= WOW64_CONTEXT_CONTROL;
        if (parts & Integer) flags |= WOW64_CONTEXT_INTEGER;
        if (parts & DebugRegisters) flags |= WOW64_CONTEXT_DEBUG_REGISTERS;
        return flags;
    }

    bool wow64_{};
    CONTEXT native_{};
    WOW64_CONTEXT wow_{};
};

} // namespace

GlobalHotkeys::~GlobalHotkeys() {
    unregisterAll();
}

std::optional<int> GlobalHotkeys::idFor(const std::string& name) {
    if (name.size() < 2 || (name.front() != 'F' && name.front() != 'f')) {
        return std::nullopt;
    }
    const auto digits = name.substr(1);
    if (!std::all_of(digits.begin(), digits.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return std::nullopt;
    }
    const int id = std::atoi(digits.c_str());
    if (id < firstId || id > lastId) {
        return std::nullopt;
    }
    return id;
}

std::vector<int> GlobalHotkeys::apply(HWND window, const std::set<int>& wanted) {
    unregisterAll();
    window_ = window;
    if (window_ == nullptr) {
        return {std::vector<int>(wanted.begin(), wanted.end())};
    }

    std::vector<int> refused;
    for (const int id : wanted) {
        if (id < firstId || id > lastId) {
            continue;
        }
        // MOD_NOREPEAT: holding the key down should toggle a freeze once, not
        // thirty times a second for as long as it is held.
        const UINT key = VK_F1 + static_cast<UINT>(id - firstId);
        if (RegisterHotKey(window_, id, MOD_NOREPEAT, key)) {
            registered_.insert(id);
        } else {
            refused.push_back(id);
        }
    }
    return refused;
}

void GlobalHotkeys::unregisterAll() {
    if (window_ != nullptr) {
        for (const int id : registered_) {
            UnregisterHotKey(window_, id);
        }
    }
    registered_.clear();
}

DebugEventPump::~DebugEventPump() {
    detach();
}

infra::Result<void> DebugEventPump::attach(std::uint32_t pid, HitCallback onHit, ExitCallback onExit) {
    detach();

    pid_ = pid;
    onHit_ = std::move(onHit);
    onExit_ = std::move(onExit);
    stop_ = false;
    targetExited_ = false;

    // DebugActiveProcess, WaitForDebugEvent, ContinueDebugEvent and
    // DebugActiveProcessStop must all be called from the same thread. Attaching
    // here and pumping elsewhere is why no debug event ever arrived.
    std::promise<infra::Result<void>> ready;
    auto started = ready.get_future();
    worker_ = std::jthread([this, promise = std::move(ready)]() mutable { run(std::move(promise)); });

    auto result = started.get();
    if (!result) {
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    return result;
}

void DebugEventPump::detach() {
    stop_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    stop_ = false;

    std::scoped_lock lock(mutex_);
    breakpoints_.clear();
    stepping_.clear();
    hardwareUsed_ = false;
}

void DebugEventPump::run(std::promise<infra::Result<void>> ready) {
    // The pump keeps its own handle. Borrowing the UI session's would mean the
    // user detaching from the process pulled the handle out from under an
    // in-flight breakpoint.
    process_.reset(OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                               FALSE, pid_));
    if (!process_) {
        const DWORD error = GetLastError();
        ready.set_value(infra::Result<void>::fail(
            "Could not open the target for debugging: " + Win32Platform::formatLastError(error), error));
        return;
    }

    // Settled before any debug event can arrive. A process cannot change
    // bitness while it runs, so this is asked once rather than per event.
    wow64_ = Win32Platform::isWow64(process_.get());

    if (!DebugActiveProcess(pid_)) {
        const DWORD error = GetLastError();
        process_.reset();
        ready.set_value(infra::Result<void>::fail(
            "Could not attach the debugger: " + Win32Platform::formatLastError(error), error));
        return;
    }

    // Without this, detaching or closing Pointer Lab kills the target outright.
    if (!DebugSetProcessKillOnExit(FALSE)) {
        infra::Logger::instance().warn("Could not clear kill-on-exit; closing Pointer Lab may terminate the target: " +
                                       Win32Platform::formatLastError());
    }

    attached_ = true;
    infra::Logger::instance().info("Debugger attached to pid " + std::to_string(pid_) + ".");
    ready.set_value(infra::Result<void>::ok());

    loop();

    // Order matters on the way out. Restore every original byte first so no new
    // trap can fire, then flush the events Windows has already queued, and only
    // then let go. Detaching with any of that outstanding kills the target.
    disarmAll();
    if (!targetExited_) {
        drainPendingEvents();
    }
    if (!targetExited_) {
        DebugActiveProcessStop(pid_);
    }
    {
        std::scoped_lock lock(mutex_);
        stepping_.clear();
        disarmed_.clear();
    }
    attached_ = false;
    process_.reset();
    infra::Logger::instance().info("Debugger detached.");
}

void DebugEventPump::loop() {
    bool sawInitialBreakpoint = false;

    // Threads mid-step and events already queued are handled by the drain in
    // run(), so stopping here can be immediate.
    while (!stop_) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 100)) {
            continue;
        }

        DWORD continueStatus = DBG_CONTINUE;
        bool exiting = false;
        DWORD exitCode = 0;

        switch (event.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            const auto& record = event.u.Exception.ExceptionRecord;
            const auto address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);

            if (record.ExceptionCode == EXCEPTION_BREAKPOINT) {
                if (handleBreakpoint(address, event.dwThreadId)) {
                    continueStatus = DBG_CONTINUE;
                } else if (!sawInitialBreakpoint) {
                    // The loader's own breakpoint, raised once when a debugger
                    // attaches. It has to be swallowed; passing it through
                    // terminates the target.
                    sawInitialBreakpoint = true;
                    continueStatus = DBG_CONTINUE;
                } else {
                    // Someone else's int3. It belongs to the target.
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                }
            } else if (record.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                continueStatus = handleSingleStep(event.dwThreadId) ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED;
            } else {
                // Every other exception is the target's own business.
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }
        case CREATE_PROCESS_DEBUG_EVENT:
            // This file handle is the debugger's to close. Leaking one per
            // attach kept the target's image file locked.
            if (event.u.CreateProcessInfo.hFile != nullptr) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
            break;
        case CREATE_THREAD_DEBUG_EVENT: {
            // Debug registers are per-thread, so a thread created after a
            // hardware breakpoint was set starts with none and the breakpoint
            // simply would not exist on it. The thread is still suspended until
            // we continue this event, which makes now the moment to program it.
            // The handle belongs to the system; unlike hFile it is not ours to
            // close.
            std::scoped_lock lock(mutex_);
            if (anyHardwareArmed() && event.u.CreateThread.hThread != nullptr &&
                !writeDebugRegisters(event.u.CreateThread.hThread)) {
                infra::Logger::instance().warn("Could not program the debug registers of new thread " +
                                               std::to_string(event.dwThreadId) +
                                               "; hardware breakpoints will not fire on it: " +
                                               Win32Platform::formatLastError());
            }
            break;
        }
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile != nullptr) {
                CloseHandle(event.u.LoadDll.hFile);
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            exiting = true;
            exitCode = event.u.ExitProcess.dwExitCode;
            break;
        default:
            break;
        }

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);

        if (exiting) {
            targetExited_ = true;
            {
                std::scoped_lock lock(mutex_);
                breakpoints_.clear();
                stepping_.clear();
            }
            if (onExit_) {
                onExit_(exitCode);
            }
            break;
        }
    }
}

bool DebugEventPump::rewindThread(std::uintptr_t address, std::uint32_t threadId) const {
    UniqueHandle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId));
    if (!thread) {
        return false;
    }
    ThreadContext context(wow64_);
    if (!context.get(thread.get(), ThreadContext::Control)) {
        return false;
    }
    context.setInstructionPointer(address);
    return context.set(thread.get(), ThreadContext::Control);
}

bool DebugEventPump::anyHardwareArmed() const {
    return std::any_of(breakpoints_.begin(), breakpoints_.end(), [](const auto& pair) {
        return domain::isHardware(pair.second.kind) && pair.second.enabled;
    });
}

int DebugEventPump::freeDebugSlot() const {
    std::array<bool, debugSlotCount> taken{};
    for (const auto& [address, info] : breakpoints_) {
        if (domain::isHardware(info.kind) && info.slot >= 0 && info.slot < debugSlotCount) {
            taken[static_cast<std::size_t>(info.slot)] = true;
        }
    }
    for (int slot = 0; slot < debugSlotCount; ++slot) {
        if (!taken[static_cast<std::size_t>(slot)]) {
            return slot;
        }
    }
    return -1;
}

bool DebugEventPump::writeDebugRegisters(HANDLE thread) const {
    ThreadContext context(wow64_);
    if (!context.get(thread, ThreadContext::DebugRegisters)) {
        return false;
    }

    std::array<DWORD64, debugSlotCount> addresses{};
    DWORD64 control = 0;
    for (const auto& [address, info] : breakpoints_) {
        if (!domain::isHardware(info.kind) || !info.enabled || info.slot < 0 || info.slot >= debugSlotCount) {
            continue;
        }
        const auto slot = static_cast<std::size_t>(info.slot);
        addresses[slot] = address;
        // Local enable. Global enable would survive a task switch, which is not
        // ours to impose on the whole machine.
        control |= DWORD64{1} << (slot * 2);
        control |= readWriteBits(info.kind) << (16 + slot * 4);
        control |= lengthBits(info.length) << (18 + slot * 4);
    }

    // programDebugRegisters also clears DR6: stale status bits would make the
    // next exception look like a hit from a register that has since been reused.
    context.programDebugRegisters(addresses, control);
    return context.set(thread, ThreadContext::DebugRegisters);
}

void DebugEventPump::applyDebugRegistersToAllThreads() const {
    for (const auto threadId : threadIdsOf(pid_)) {
        UniqueHandle thread(
            OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId));
        if (!thread) {
            infra::Logger::instance().warn("Could not open thread " + std::to_string(threadId) +
                                           " to program its debug registers; a hardware breakpoint will not fire on "
                                           "it: " + Win32Platform::formatLastError());
            continue;
        }
        // Reading a running thread's context returns whatever the kernel last
        // saved for it, which is not necessarily current. Suspending first is
        // what makes the read-modify-write meaningful.
        const bool suspended = SuspendThread(thread.get()) != static_cast<DWORD>(-1);
        if (!writeDebugRegisters(thread.get())) {
            infra::Logger::instance().warn("Could not program the debug registers of thread " +
                                           std::to_string(threadId) + ": " + Win32Platform::formatLastError());
        }
        if (suspended) {
            ResumeThread(thread.get());
        }
    }
}

bool DebugEventPump::handleBreakpoint(std::uintptr_t address, std::uint32_t threadId) {
    domain::BreakpointInfo snapshot;
    {
        std::scoped_lock lock(mutex_);
        auto entry = breakpoints_.find(address);
        if (entry == breakpoints_.end() || !entry->second.enabled) {
            // A trap we armed and have since removed can still be in flight:
            // the thread executed the int3 microseconds before the original
            // byte went back. Its RIP still needs rewinding, and the exception
            // is still ours to swallow - passing it on kills the target.
            if (disarmed_.count(address) != 0) {
                if (!rewindThread(address, threadId)) {
                    infra::Logger::instance().error("Could not rewind a thread off a removed breakpoint at " +
                                                    domain::toHex(address) + ".");
                }
                return true;
            }
            return false;
        }

        UniqueHandle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId));
        ThreadContext context(wow64_);
        if (!thread || !context.get(thread.get(), ThreadContext::Control | ThreadContext::Integer)) {
            infra::Logger::instance().error("Breakpoint at " + domain::toHex(address) +
                                            " was hit but its thread could not be read: " +
                                            Win32Platform::formatLastError());
            // Still ours, so still swallowed. Letting it through would be a
            // certain kill rather than a possible one.
            return true;
        }

        // int3 has already executed, so the instruction pointer sits one byte
        // past the breakpoint. Resuming from there runs the tail of a
        // half-decoded instruction, which is exactly what used to crash the
        // target.
        context.setInstructionPointer(address);
        context.capture(threadId, entry->second.lastHit);

        // Put the original instruction back so the thread can execute it, and
        // arm the trap flag so we get control again immediately afterwards.
        if (auto restored = writeByte(address, entry->second.originalByte); !restored) {
            infra::Logger::instance().error("Could not restore the original byte at " + domain::toHex(address) + ": " +
                                            restored.error());
            return true;
        }
        context.orFlags(trapFlag);

        if (!context.set(thread.get(), ThreadContext::Control | ThreadContext::Integer)) {
            infra::Logger::instance().error("Could not rewind the thread at " + domain::toHex(address) + ": " +
                                            Win32Platform::formatLastError());
            // Leave the original byte in place: the breakpoint stops working,
            // but the target keeps running, which is the better failure.
            disarmed_.insert(address);
            breakpoints_.erase(entry);
            return true;
        }

        stepping_[threadId] = address;
        entry->second.hitCount += 1;
        snapshot = entry->second;
    }

    // Called without the lock: the callback reaches into the UI, which takes
    // locks of its own.
    if (onHit_) {
        onHit_(snapshot);
    }
    return true;
}

bool DebugEventPump::handleSingleStep(std::uint32_t threadId) {
    domain::BreakpointInfo snapshot;
    bool hardwareHit = false;
    bool handled = false;

    {
        std::scoped_lock lock(mutex_);

        // A debug register fires as a single-step exception, so this is also the
        // hardware breakpoint path. DR6 names the register that caused it;
        // without asking, a watchpoint hit looks like the target stepping itself
        // and gets passed straight through, which kills it.
        if (hardwareUsed_) {
            const auto verdict = handleHardwareHit(threadId, snapshot);
            hardwareHit = verdict == HardwareVerdict::Hit;
            handled = verdict != HardwareVerdict::NotOurs;
        }

        if (auto stepping = stepping_.find(threadId); stepping != stepping_.end()) {
            handled = handleSoftwareStep(stepping) || handled;
        }

        // Nothing claimed it, but debug registers were in use at some point in
        // this attach. A trap that was already in flight when its register was
        // cleared arrives with DR6 wiped and no owner to match, which is exactly
        // this case -- and it is the shape of the bug that used to kill the
        // target on detach. Swallowing somebody else's single step costs
        // nothing; passing ours on is a certain kill.
        if (!handled && hardwareUsed_) {
            handled = true;
        }
    }

    // Called without the lock: the callback reaches into the UI, which takes
    // locks of its own.
    if (hardwareHit && onHit_) {
        onHit_(snapshot);
    }
    return handled;
}

// Both halves below expect mutex_ to be held.

DebugEventPump::HardwareVerdict DebugEventPump::handleHardwareHit(std::uint32_t threadId,
                                                                  domain::BreakpointInfo& snapshot) {
    UniqueHandle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId));
    if (!thread) {
        return HardwareVerdict::NotOurs;
    }

    ThreadContext context(wow64_);
    if (!context.get(thread.get(),
                     ThreadContext::DebugRegisters | ThreadContext::Control | ThreadContext::Integer)) {
        return HardwareVerdict::NotOurs;
    }

    const DWORD64 fired = context.debugStatus() & debugStatusMask;
    if (fired == 0) {
        // A real single step, or somebody else's. Not ours to claim.
        return HardwareVerdict::NotOurs;
    }

    domain::BreakpointInfo* hit = nullptr;
    for (auto& [address, info] : breakpoints_) {
        if (!domain::isHardware(info.kind) || !info.enabled || info.slot < 0) {
            continue;
        }
        if ((fired & (DWORD64{1} << static_cast<unsigned>(info.slot))) != 0) {
            hit = &info;
            break;
        }
    }
    if (hit == nullptr) {
        // A register we no longer own reported in: the trap was raised before
        // the breakpoint was removed and its exception arrived afterwards. Still
        // ours, so still swallowed -- and the status bits have to be cleared or
        // it reports again on every later exception.
        context.clearDebugStatus();
        if (!context.set(thread.get(), ThreadContext::DebugRegisters)) {
            infra::Logger::instance().warn("Could not clear DR6 after a stale hardware trap on thread " +
                                           std::to_string(threadId) + ".");
        }
        return HardwareVerdict::Stale;
    }

    context.capture(threadId, hit->lastHit);
    hit->hitCount += 1;
    snapshot = *hit;

    // An execute breakpoint faults before the instruction runs, so resuming
    // as-is would fault on it again immediately and the target would never make
    // progress. A data breakpoint traps after the access and needs none of this.
    context.clearDebugStatus();
    unsigned parts = ThreadContext::DebugRegisters;
    if (hit->kind == domain::BreakpointKind::HardwareExecute) {
        context.orFlags(resumeFlag);
        parts |= ThreadContext::Control;
    }
    if (!context.set(thread.get(), parts)) {
        infra::Logger::instance().error("A hardware breakpoint at " + domain::toHex(hit->address) +
                                        " was hit but its thread could not be resumed cleanly: " +
                                        Win32Platform::formatLastError());
    }
    return HardwareVerdict::Hit;
}

bool DebugEventPump::handleSoftwareStep(std::map<std::uint32_t, std::uintptr_t>::iterator stepping) {
    const auto address = stepping->second;
    stepping_.erase(stepping);

    // The original instruction has now executed, so put the trap back. This is
    // what makes a breakpoint fire more than once. The CPU has already cleared
    // the trap flag for us as part of raising this exception.
    auto entry = breakpoints_.find(address);
    if (entry != breakpoints_.end() && entry->second.enabled) {
        if (auto armed = writeByte(address, int3); !armed) {
            infra::Logger::instance().error("Could not re-arm the breakpoint at " + domain::toHex(address) + ": " +
                                            armed.error());
            breakpoints_.erase(entry);
        }
    }
    return true;
}

infra::Result<void> DebugEventPump::addBreakpoint(std::uintptr_t address, std::string label,
                                                  domain::BreakpointKind kind, std::uint8_t length) {
    if (!attached_) {
        return infra::Result<void>::fail("The debugger is not attached.");
    }

    std::scoped_lock lock(mutex_);
    if (breakpoints_.count(address) != 0) {
        return infra::Result<void>::fail("There is already a breakpoint at " + domain::toHex(address) + ".");
    }

    if (domain::isHardware(kind)) {
        return addHardwareBreakpoint(address, std::move(label), kind, length);
    }

    auto original = readByte(address);
    if (!original) {
        return infra::Result<void>::fail("Could not read the target byte: " + original.error(), original.code());
    }
    if (original.value() == int3) {
        // Saving 0xCC as the "original" byte would make removing the breakpoint
        // leave a permanent int3 behind.
        return infra::Result<void>::fail("There is already an int3 at " + domain::toHex(address) +
                                         "; Pointer Lab will not stack a breakpoint on top of it.");
    }

    if (auto armed = writeByte(address, int3); !armed) {
        return infra::Result<void>::fail("Could not arm the breakpoint: " + armed.error(), armed.code());
    }
    // Live again, so a trap here is no longer a stale one to be swallowed.
    disarmed_.erase(address);

    domain::BreakpointInfo info;
    info.address = address;
    info.originalByte = original.value();
    info.enabled = true;
    info.label = std::move(label);
    breakpoints_.emplace(address, std::move(info));
    return infra::Result<void>::ok();
}

infra::Result<void> DebugEventPump::addHardwareBreakpoint(std::uintptr_t address, std::string label,
                                                          domain::BreakpointKind kind, std::uint8_t length) {
    // Execute breakpoints trap an instruction fetch, which is one byte by
    // definition; only a data breakpoint has a width to choose.
    if (kind == domain::BreakpointKind::HardwareExecute) {
        length = 1;
    }
    if (!domain::isValidWatchLength(length)) {
        return infra::Result<void>::fail("A hardware breakpoint can watch 1, 2, 4 or 8 bytes, not " +
                                         std::to_string(length) + ".");
    }
    if (address % length != 0) {
        return infra::Result<void>::fail("A " + std::to_string(length) + "-byte hardware breakpoint must watch an " +
                                         std::to_string(length) + "-byte aligned address, and " +
                                         domain::toHex(address) + " is not.");
    }

    const int slot = freeDebugSlot();
    if (slot < 0) {
        return infra::Result<void>::fail(
            "All four debug registers are in use. The processor provides exactly four hardware breakpoints; "
            "remove one, or use a software breakpoint instead.");
    }

    domain::BreakpointInfo info;
    info.address = address;
    info.enabled = true;
    info.label = std::move(label);
    info.kind = kind;
    info.length = length;
    info.slot = slot;
    breakpoints_.emplace(address, std::move(info));
    hardwareUsed_ = true;

    // Programmed from the table, which now includes the new entry.
    applyDebugRegistersToAllThreads();
    return infra::Result<void>::ok();
}

infra::Result<void> DebugEventPump::removeBreakpoint(std::uintptr_t address) {
    std::scoped_lock lock(mutex_);

    auto entry = breakpoints_.find(address);
    if (entry == breakpoints_.end()) {
        return infra::Result<void>::fail("There is no breakpoint at " + domain::toHex(address) + ".");
    }

    // Nothing was written into the target, so removal is just giving the debug
    // register back and reprogramming every thread from the table.
    if (domain::isHardware(entry->second.kind)) {
        breakpoints_.erase(entry);
        applyDebugRegistersToAllThreads();
        return infra::Result<void>::ok();
    }

    // Erase first: if a thread is mid-step over this address, handleSingleStep
    // must not find it and re-arm it behind us.
    const auto originalByte = entry->second.originalByte;
    const bool steppingOver = std::any_of(stepping_.begin(), stepping_.end(),
                                          [address](const auto& pair) { return pair.second == address; });
    breakpoints_.erase(entry);
    // A thread may have executed the trap moments ago and its exception may
    // still be in flight; that event is ours to absorb, not the target's.
    disarmed_.insert(address);

    // A thread stepping over it has already had the original byte restored.
    if (!steppingOver) {
        if (auto restored = writeByte(address, originalByte); !restored) {
            return infra::Result<void>::fail("The breakpoint was removed but the original byte could not be written "
                                             "back, so the target still contains an int3: " + restored.error(),
                                             restored.code());
        }
    }
    return infra::Result<void>::ok();
}

std::vector<domain::BreakpointInfo> DebugEventPump::breakpoints() const {
    std::scoped_lock lock(mutex_);
    std::vector<domain::BreakpointInfo> result;
    result.reserve(breakpoints_.size());
    for (const auto& [address, info] : breakpoints_) {
        result.push_back(info);
    }
    return result;
}

void DebugEventPump::disarmAll() {
    std::scoped_lock lock(mutex_);
    const bool hadHardware = anyHardwareArmed();

    for (const auto& [address, info] : breakpoints_) {
        if (domain::isHardware(info.kind)) {
            // Nothing to write back: it never touched the target's memory.
            continue;
        }
        // A thread stepping over this one already has the original byte back.
        const bool steppingOver = std::any_of(stepping_.begin(), stepping_.end(),
                                              [addr = address](const auto& pair) { return pair.second == addr; });
        if (!steppingOver) {
            if (auto restored = writeByte(address, info.originalByte); !restored) {
                infra::Logger::instance().error("Could not remove the breakpoint at " + domain::toHex(address) +
                                                "; the target still contains an int3: " + restored.error());
            }
        }
        disarmed_.insert(address);
    }
    breakpoints_.clear();

    // Now that the table is empty this writes zeroed registers, which is what
    // hands the debug registers back. Leaving them set would keep faulting a
    // target that no longer has a debugger to field the exceptions.
    if (hadHardware) {
        applyDebugRegistersToAllThreads();
    }
}

void DebugEventPump::drainPendingEvents() {
    // Everything is disarmed by now, so no new trap can fire. What is left is
    // whatever Windows already queued, and every one of those must be continued
    // before letting go of the process: DebugActiveProcessStop delivers a
    // pending exception to a target that no longer has a debugger, which kills
    // it. With a breakpoint in a hot loop there is nearly always one in flight.
    const auto deadline = std::chrono::steady_clock::now() + drainTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 50)) {
            // Nothing queued for a full timeout: the target is clean.
            break;
        }

        DWORD continueStatus = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            const auto& record = event.u.Exception.ExceptionRecord;
            const auto address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
            if (record.ExceptionCode == EXCEPTION_BREAKPOINT) {
                if (!handleBreakpoint(address, event.dwThreadId)) {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                }
            } else if (record.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                continueStatus = handleSingleStep(event.dwThreadId) ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED;
            } else {
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }
        case CREATE_PROCESS_DEBUG_EVENT:
            if (event.u.CreateProcessInfo.hFile != nullptr) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile != nullptr) {
                CloseHandle(event.u.LoadDll.hFile);
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            targetExited_ = true;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            return;
        default:
            break;
        }

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
    }
}

infra::Result<std::uint8_t> DebugEventPump::readByte(std::uintptr_t address) const {
    auto bytes = platform_.readMemory(process_.get(), address, 1);
    if (!bytes) {
        return infra::Result<std::uint8_t>::fail(bytes.error(), bytes.code());
    }
    if (bytes.value().empty()) {
        return infra::Result<std::uint8_t>::fail("Nothing is mapped at " + domain::toHex(address) + ".");
    }
    return infra::Result<std::uint8_t>::ok(bytes.value().front());
}

infra::Result<void> DebugEventPump::writeByte(std::uintptr_t address, std::uint8_t value) const {
    auto written = platform_.writeMemory(process_.get(), address, &value, 1);
    if (written) {
        // Patched code can otherwise sit stale in an instruction cache.
        FlushInstructionCache(process_.get(), reinterpret_cast<LPCVOID>(address), 1);
    }
    return written;
}

} // namespace ire::platform_win32
