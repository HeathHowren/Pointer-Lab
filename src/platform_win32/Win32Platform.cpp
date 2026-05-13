#include "platform_win32/Win32Platform.h"

#include "infra/Logger.h"

#include <TlHelp32.h>
#include <processthreadsapi.h>

#include <algorithm>
#include <array>
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

infra::Result<UniqueHandle> Win32Platform::openProcess(std::uint32_t pid) const {
    const DWORD rights =
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
        PROCESS_CREATE_THREAD | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;

    HANDLE process = OpenProcess(rights, FALSE, pid);
    if (!process) {
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    }
    if (!process) {
        return infra::Result<UniqueHandle>::fail(formatLastError());
    }
    return infra::Result<UniqueHandle>::ok(UniqueHandle(process));
}

infra::Result<std::vector<std::uint8_t>> Win32Platform::readMemory(HANDLE process, std::uintptr_t address, std::size_t size) const {
    std::vector<std::uint8_t> bytes(size);
    SIZE_T read{};
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), bytes.data(), size, &read)) {
        if (read == 0) {
            return infra::Result<std::vector<std::uint8_t>>::fail(formatLastError());
        }
    }
    bytes.resize(read);
    return infra::Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

infra::Result<void> Win32Platform::writeMemory(HANDLE process, std::uintptr_t address, const void* data, std::size_t size) const {
    DWORD oldProtection{};
    VirtualProtectEx(process, reinterpret_cast<LPVOID>(address), size, PAGE_EXECUTE_READWRITE, &oldProtection);
    SIZE_T written{};
    const BOOL ok = WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), data, size, &written);
    FlushInstructionCache(process, reinterpret_cast<LPCVOID>(address), size);
    if (oldProtection != 0) {
        DWORD ignored{};
        VirtualProtectEx(process, reinterpret_cast<LPVOID>(address), size, oldProtection, &ignored);
    }
    if (!ok || written != size) {
        return infra::Result<void>::fail(formatLastError());
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

infra::Result<void> Win32Platform::free(HANDLE process, std::uintptr_t address) const {
    if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(address), 0, MEM_RELEASE)) {
        return infra::Result<void>::fail(formatLastError());
    }
    return infra::Result<void>::ok();
}

infra::Result<std::uint32_t> Win32Platform::createRemoteThread(HANDLE process, std::uintptr_t start, std::uintptr_t parameter) const {
    UniqueHandle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), reinterpret_cast<LPVOID>(parameter), 0, nullptr));
    if (!thread) {
        return infra::Result<std::uint32_t>::fail(formatLastError());
    }
    WaitForSingleObject(thread.get(), 5000);
    DWORD exitCode{};
    GetExitCodeThread(thread.get(), &exitCode);
    return infra::Result<std::uint32_t>::ok(exitCode);
}

infra::Result<std::uint32_t> Win32Platform::injectLoadLibraryW(HANDLE process, const std::wstring& dllPath) const {
    const auto bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    auto remote = allocate(process, bytes, PAGE_READWRITE);
    if (!remote) {
        return infra::Result<std::uint32_t>::fail(remote.error());
    }
    auto write = writeMemory(process, remote.value(), dllPath.c_str(), bytes);
    if (!write) {
        free(process, remote.value());
        return infra::Result<std::uint32_t>::fail(write.error());
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<std::uintptr_t>(GetProcAddress(kernel32, "LoadLibraryW"));
    auto result = createRemoteThread(process, loadLibrary, remote.value());
    free(process, remote.value());
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

DebugEventPump::~DebugEventPump() {
    detach();
}

infra::Result<void> DebugEventPump::attach(std::uint32_t pid, BreakpointHitCallback callback) {
    detach();
    if (!DebugActiveProcess(pid)) {
        return infra::Result<void>::fail(Win32Platform::formatLastError());
    }
    if (!DebugSetProcessKillOnExit(FALSE)) {
        DebugActiveProcessStop(pid);
        return infra::Result<void>::fail(Win32Platform::formatLastError());
    }
    pid_ = pid;
    callback_ = std::move(callback);
    stop_ = false;
    attached_ = true;
    thread_ = CreateThread(nullptr, 0, [](LPVOID self) -> DWORD {
        static_cast<DebugEventPump*>(self)->loop();
        return 0;
    }, this, 0, nullptr);
    infra::Logger::instance().info("Debugger attached.");
    return infra::Result<void>::ok();
}

void DebugEventPump::detach() {
    if (!attached_) {
        return;
    }
    stop_ = true;
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    DebugActiveProcessStop(pid_);
    attached_ = false;
    infra::Logger::instance().info("Debugger detached.");
}

void DebugEventPump::loop() {
    while (!stop_) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 100)) {
            continue;
        }

        DWORD continueStatus = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& exception = event.u.Exception.ExceptionRecord;
            if (exception.ExceptionCode == EXCEPTION_BREAKPOINT) {
                const auto address = reinterpret_cast<std::uintptr_t>(exception.ExceptionAddress);
                if (callback_) {
                    callback_(address);
                }
            } else {
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
        }

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
    }
}

} // namespace ire::platform_win32
