#include "services/RuntimeServices.h"

#include "infra/Logger.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace ire::services {

AddressListService::AddressListService(domain::TargetSession& session) : session_(session) {
    worker_ = std::jthread([this](std::stop_token token) { freezeLoop(token); });
}

AddressListService::~AddressListService() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

std::uint64_t AddressListService::add(std::uintptr_t address, domain::ValueType type, std::string description, std::string group) {
    domain::AddressEntry entry;
    entry.address = address;
    entry.type = type;
    entry.description = std::move(description);
    entry.group = std::move(group);
    const auto size = domain::valueTypeSize(type);
    if (size > 0 && session_.attached()) {
        if (auto bytes = session_.readBytes(address, size)) {
            entry.frozenValue = std::move(bytes.value());
        }
    }
    return session_.addressList().add(std::move(entry));
}

bool AddressListService::remove(std::uint64_t id) {
    return session_.addressList().remove(id);
}

bool AddressListService::setFrozen(std::uint64_t id, bool frozen) {
    auto entries = session_.addressList().snapshot();
    for (auto& entry : entries) {
        if (entry.id == id) {
            if (frozen && entry.frozenValue.empty() && session_.attached()) {
                if (auto bytes = session_.readBytes(entry.address, domain::valueTypeSize(entry.type))) {
                    entry.frozenValue = std::move(bytes.value());
                }
            }
            entry.frozen = frozen;
            return session_.addressList().update(entry);
        }
    }
    return false;
}

bool AddressListService::updateValue(std::uint64_t id, const std::vector<std::uint8_t>& bytes) {
    auto entries = session_.addressList().snapshot();
    for (auto& entry : entries) {
        if (entry.id == id) {
            entry.frozenValue = bytes;
            session_.addressList().update(entry);
            if (session_.attached()) {
                return static_cast<bool>(session_.writeBytes(entry.address, bytes));
            }
            return true;
        }
    }
    return false;
}

void AddressListService::toggleHotkey(const std::string& hotkey) {
    auto entries = session_.addressList().snapshot();
    for (auto& entry : entries) {
        if (_stricmp(entry.hotkey.c_str(), hotkey.c_str()) == 0) {
            entry.frozen = !entry.frozen;
            session_.addressList().update(entry);
            infra::Logger::instance().info("Hotkey " + hotkey + " toggled freeze for " + entry.description + ".");
        }
    }
}

void AddressListService::freezeLoop(std::stop_token token) {
    while (!token.stop_requested()) {
        if (session_.attached()) {
            for (const auto& entry : session_.addressList().snapshot()) {
                if (entry.frozen && !entry.frozenValue.empty()) {
                    session_.writeBytes(entry.address, entry.frozenValue);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

BreakpointService::BreakpointService(domain::TargetSession& session) : session_(session) {}

BreakpointService::~BreakpointService() {
    detachDebugger();
}

infra::Result<void> BreakpointService::attachDebugger() {
    if (!session_.attached()) {
        return infra::Result<void>::fail("No target process attached.");
    }
    if (debugPump_.attached()) {
        return infra::Result<void>::ok();
    }
    return debugPump_.attach(session_.pid(), [this](std::uintptr_t address) { onBreakpointHit(address); });
}

void BreakpointService::detachDebugger() {
    std::vector<std::uintptr_t> addresses;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [address, _] : breakpoints_) {
            addresses.push_back(address);
        }
    }
    for (const auto address : addresses) {
        removeBreakpoint(address);
    }
    debugPump_.detach();
}

infra::Result<void> BreakpointService::addBreakpoint(std::uintptr_t address, std::string label) {
    auto attach = attachDebugger();
    if (!attach) {
        return attach;
    }

    auto original = session_.readBytes(address, 1);
    if (!original || original.value().size() != 1) {
        return infra::Result<void>::fail(original ? "Could not read original byte." : original.error());
    }
    std::vector<std::uint8_t> int3{0xCC};
    auto write = session_.writeBytes(address, int3);
    if (!write) {
        return write;
    }

    domain::BreakpointInfo bp;
    bp.address = address;
    bp.originalByte = original.value()[0];
    bp.enabled = true;
    bp.label = std::move(label);
    {
        std::scoped_lock lock(mutex_);
        breakpoints_[address] = bp;
    }
    infra::Logger::instance().info("Breakpoint set at " + domain::toHex(address) + ".");
    return infra::Result<void>::ok();
}

infra::Result<void> BreakpointService::removeBreakpoint(std::uintptr_t address) {
    domain::BreakpointInfo bp;
    {
        std::scoped_lock lock(mutex_);
        auto it = breakpoints_.find(address);
        if (it == breakpoints_.end()) {
            return infra::Result<void>::ok();
        }
        bp = it->second;
        breakpoints_.erase(it);
    }

    if (bp.enabled && session_.attached()) {
        std::vector<std::uint8_t> original{bp.originalByte};
        session_.writeBytes(address, original);
    }
    infra::Logger::instance().info("Breakpoint removed at " + domain::toHex(address) + ".");
    return infra::Result<void>::ok();
}

std::vector<domain::BreakpointInfo> BreakpointService::breakpoints() const {
    std::vector<domain::BreakpointInfo> items;
    std::scoped_lock lock(mutex_);
    for (const auto& [_, bp] : breakpoints_) {
        items.push_back(bp);
    }
    return items;
}

bool BreakpointService::debuggerAttached() const {
    return debugPump_.attached();
}

void BreakpointService::onBreakpointHit(std::uintptr_t address) {
    std::uintptr_t key = address;
    {
        std::scoped_lock lock(mutex_);
        if (!breakpoints_.contains(key) && breakpoints_.contains(address - 1)) {
            key = address - 1;
        }
        auto it = breakpoints_.find(key);
        if (it == breakpoints_.end()) {
            return;
        }
        it->second.hitCount++;
        it->second.enabled = false;
        std::vector<std::uint8_t> original{it->second.originalByte};
        session_.writeBytes(key, original);
    }
    infra::Logger::instance().info("Breakpoint hit at " + domain::toHex(key) + ". It was restored and disabled.");
}

RuntimeServices::RuntimeServices()
    : session_(platform_),
      addressList_(session_),
      scanJob_(session_, {}),
      luaScanJob_(session_),
      pointerScanJob_(session_),
      injector_(session_),
      breakpoints_(session_) {}

} // namespace ire::services
