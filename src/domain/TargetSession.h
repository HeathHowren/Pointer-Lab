#pragma once

#include "domain/AddressList.h"
#include "infra/Result.h"
#include "platform_win32/Win32Platform.h"

#include <mutex>
#include <optional>

namespace ire::domain {

class TargetSession {
public:
    explicit TargetSession(platform_win32::Win32Platform& platform);
    ~TargetSession();

    infra::Result<void> attach(std::uint32_t pid);
    void detach();
    void refresh();

    [[nodiscard]] bool attached() const;
    // True when only a read-only handle could be obtained, so every write,
    // freeze, patch and injection attempt will fail.
    [[nodiscard]] bool readOnly() const;
    [[nodiscard]] std::uint32_t pid() const;
    [[nodiscard]] std::wstring processName() const;
    [[nodiscard]] HANDLE processHandle() const;

    // Pointer width of the attached target, determined once at attach. Returns
    // X64 when nothing is attached, which is the harmless default: every engine
    // that consults it also checks attached() first.
    [[nodiscard]] Bitness bitness() const;
    [[nodiscard]] std::size_t pointerSize() const;

    // Reads one target-width pointer. Every chain walk and pointer scan goes
    // through this rather than memcpy-ing sizeof(std::uintptr_t), which read 8
    // bytes out of a 32-bit process and found nothing.
    infra::Result<std::uintptr_t> readPointer(std::uintptr_t address) const;

    [[nodiscard]] std::vector<ModuleInfo> modules() const;
    [[nodiscard]] std::vector<MemoryRegion> regions() const;

    infra::Result<std::vector<std::uint8_t>> readBytes(std::uintptr_t address, std::size_t size) const;
    infra::Result<void> writeBytes(std::uintptr_t address, const std::vector<std::uint8_t>& bytes) const;

    AddressList& addressList() { return addressList_; }
    const AddressList& addressList() const { return addressList_; }
    platform_win32::Win32Platform& platform() { return platform_; }
    const platform_win32::Win32Platform& platform() const { return platform_; }

private:
    platform_win32::Win32Platform& platform_;
    mutable std::mutex mutex_;
    platform_win32::UniqueHandle process_;
    std::uint32_t pid_{};
    bool readOnly_{};
    Bitness bitness_{Bitness::X64};
    std::wstring processName_;
    std::vector<ModuleInfo> modules_;
    std::vector<MemoryRegion> regions_;
    AddressList addressList_;
};

} // namespace ire::domain

