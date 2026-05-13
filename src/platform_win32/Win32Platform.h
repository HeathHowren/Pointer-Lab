#pragma once

#include "domain/Domain.h"
#include "infra/Result.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
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

    infra::Result<UniqueHandle> openProcess(std::uint32_t pid) const;
    infra::Result<std::vector<std::uint8_t>> readMemory(HANDLE process, std::uintptr_t address, std::size_t size) const;
    infra::Result<void> writeMemory(HANDLE process, std::uintptr_t address, const void* data, std::size_t size) const;
    infra::Result<void> protectMemory(HANDLE process, std::uintptr_t address, std::size_t size, DWORD protection, DWORD* oldProtection) const;
    infra::Result<std::uintptr_t> allocate(HANDLE process, std::size_t size, DWORD protection) const;
    infra::Result<void> free(HANDLE process, std::uintptr_t address) const;
    infra::Result<std::uint32_t> createRemoteThread(HANDLE process, std::uintptr_t start, std::uintptr_t parameter) const;
    infra::Result<std::uint32_t> injectLoadLibraryW(HANDLE process, const std::wstring& dllPath) const;

    static bool isReadableProtect(DWORD protect);
    static bool isWritableProtect(DWORD protect);
    static bool isExecutableProtect(DWORD protect);
    static std::string formatLastError(DWORD error = GetLastError());
    static std::string protectToString(DWORD protect);
};

class DebugEventPump {
public:
    using BreakpointHitCallback = std::function<void(std::uintptr_t)>;

    DebugEventPump() = default;
    ~DebugEventPump();

    DebugEventPump(const DebugEventPump&) = delete;
    DebugEventPump& operator=(const DebugEventPump&) = delete;

    infra::Result<void> attach(std::uint32_t pid, BreakpointHitCallback callback);
    void detach();
    [[nodiscard]] bool attached() const { return attached_; }

private:
    void loop();

    std::uint32_t pid_{};
    bool attached_{};
    bool stop_{};
    BreakpointHitCallback callback_;
    HANDLE thread_{nullptr};
};

} // namespace ire::platform_win32

