#include "domain/TargetSession.h"

#include "infra/Logger.h"

namespace ire::domain {

TargetSession::TargetSession(platform_win32::Win32Platform& platform) : platform_(platform) {}

TargetSession::~TargetSession() {
    detach();
}

infra::Result<void> TargetSession::attach(std::uint32_t pid) {
    bool limitedAccess = false;
    auto process = platform_.openProcess(pid, &limitedAccess);
    if (!process) {
        return infra::Result<void>::fail(process.error(), process.code());
    }

    std::wstring name;
    for (const auto& candidate : platform_.listProcesses()) {
        if (candidate.pid == pid) {
            name = candidate.name;
            break;
        }
    }

    {
        std::scoped_lock lock(mutex_);
        process_ = std::move(process.value());
        pid_ = pid;
        processName_ = std::move(name);
        modules_ = platform_.listModules(pid_);
        regions_ = platform_.listMemoryRegions(process_.get());
        readOnly_ = limitedAccess;
    }

    infra::Logger::instance().info("Attached to process " + std::to_string(pid) + ".");
    if (limitedAccess) {
        // The attach genuinely succeeded, so this is a warning rather than a
        // failure - but it used to be completely silent, and every later write
        // then failed one at a time with no hint at the underlying cause.
        infra::Logger::instance().warn(
            "Attached to process " + std::to_string(pid) + " with read-only access. "
            "Writing, freezing, patching and injection will fail. "
            "Run Pointer Lab as administrator for full access.");
    }
    return infra::Result<void>::ok();
}

void TargetSession::detach() {
    std::scoped_lock lock(mutex_);
    if (process_) {
        infra::Logger::instance().info("Detached from process " + std::to_string(pid_) + ".");
    }
    process_.reset();
    pid_ = 0;
    readOnly_ = false;
    processName_.clear();
    modules_.clear();
    regions_.clear();
}

void TargetSession::refresh() {
    std::scoped_lock lock(mutex_);
    if (!process_) {
        return;
    }
    modules_ = platform_.listModules(pid_);
    regions_ = platform_.listMemoryRegions(process_.get());
}

bool TargetSession::attached() const {
    std::scoped_lock lock(mutex_);
    return static_cast<bool>(process_);
}

bool TargetSession::readOnly() const {
    std::scoped_lock lock(mutex_);
    return static_cast<bool>(process_) && readOnly_;
}

std::uint32_t TargetSession::pid() const {
    std::scoped_lock lock(mutex_);
    return pid_;
}

std::wstring TargetSession::processName() const {
    std::scoped_lock lock(mutex_);
    return processName_;
}

HANDLE TargetSession::processHandle() const {
    std::scoped_lock lock(mutex_);
    return process_.get();
}

std::vector<ModuleInfo> TargetSession::modules() const {
    std::scoped_lock lock(mutex_);
    return modules_;
}

std::vector<MemoryRegion> TargetSession::regions() const {
    std::scoped_lock lock(mutex_);
    return regions_;
}

// The lock is held across the platform call. Copying the raw HANDLE out and
// releasing the lock first left a window where detach() could close it while a
// scan or the freeze thread was still using it.
infra::Result<std::vector<std::uint8_t>> TargetSession::readBytes(std::uintptr_t address, std::size_t size) const {
    std::scoped_lock lock(mutex_);
    if (!process_) {
        return infra::Result<std::vector<std::uint8_t>>::fail("No target process attached.");
    }
    return platform_.readMemory(process_.get(), address, size);
}

infra::Result<void> TargetSession::writeBytes(std::uintptr_t address, const std::vector<std::uint8_t>& bytes) const {
    std::scoped_lock lock(mutex_);
    if (!process_) {
        return infra::Result<void>::fail("No target process attached.");
    }
    return platform_.writeMemory(process_.get(), address, bytes.data(), bytes.size());
}

} // namespace ire::domain

