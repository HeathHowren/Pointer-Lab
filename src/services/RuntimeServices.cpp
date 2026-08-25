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

std::uint64_t AddressListService::addChain(domain::PointerChain chain, domain::ValueType type,
                                           std::string description, std::string group) {
    domain::AddressEntry entry;
    entry.type = type;
    entry.description = std::move(description);
    entry.group = std::move(group);

    // Resolve once up front so the entry has a usable address immediately
    // rather than showing nothing until the next background pass.
    if (auto resolved = engine_pointer::resolveChain(session_, chain)) {
        entry.address = resolved.value();
        entry.resolved = true;
        if (const auto size = domain::valueTypeSize(type); size > 0) {
            if (auto bytes = session_.readBytes(entry.address, size)) {
                entry.frozenValue = std::move(bytes.value());
            }
        }
    } else {
        entry.address = chain.scanTimeBase();
        entry.resolved = false;
        infra::Logger::instance().warn("Pointer chain did not resolve when it was added: " + resolved.error());
    }

    entry.chain = std::move(chain);
    return session_.addressList().add(std::move(entry));
}

void AddressListService::resolvePointerChains() {
    if (!session_.attached()) {
        return;
    }
    for (auto entry : session_.addressList().snapshot()) {
        if (!entry.chain) {
            continue;
        }
        auto resolved = engine_pointer::resolveChain(session_, *entry.chain);
        const bool ok = resolved.has_value();
        const auto address = ok ? resolved.value() : entry.address;
        if (ok == entry.resolved && address == entry.address) {
            continue;
        }
        if (!ok && entry.resolved) {
            infra::Logger::instance().warn("Pointer chain for \"" + entry.description + "\" stopped resolving: " +
                                           resolved.error());
        }
        entry.address = address;
        entry.resolved = ok;
        session_.addressList().update(entry);
    }
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
    int tick = 0;
    while (!token.stop_requested()) {
        if (session_.attached()) {
            // Chains are re-resolved far less often than freezes are written:
            // each one costs a read per level, and pointers move on the scale of
            // level loads, not milliseconds.
            if (tick % resolveEveryTicks == 0) {
                resolvePointerChains();
            }

            for (const auto& entry : session_.addressList().snapshot()) {
                if (!entry.frozen || entry.frozenValue.empty() || !entry.resolved) {
                    continue;
                }
                auto written = session_.writeBytes(entry.address, entry.frozenValue);
                if (written) {
                    continue;
                }
                // Dropping this Result meant a freeze on a read-only or
                // unmapped page looked exactly like a working one. Report once
                // and switch the entry off rather than failing twenty times a
                // second forever.
                infra::Logger::instance().error(
                    "Freeze failed at " + domain::toHex(entry.address) + " (" + written.error() +
                    "). Unfreezing this entry.");
                auto current = entry;
                current.frozen = false;
                session_.addressList().update(current);
            }
        }
        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(freezeIntervalMs));
    }
}

BreakpointService::BreakpointService(domain::TargetSession& session) : session_(session) {}

BreakpointService::~BreakpointService() {
    detachDebugger();
}

infra::Result<void> BreakpointService::attachDebugger() {
    if (!session_.attached()) {
        return infra::Result<void>::fail("No target process is attached.");
    }
    if (debugPump_.attached()) {
        return infra::Result<void>::ok();
    }

    return debugPump_.attach(
        session_.pid(),
        [this](const domain::BreakpointInfo& info) {
            std::string message = "Breakpoint hit at " + domain::toHex(info.address);
            if (!info.label.empty()) {
                message += " (" + info.label + ")";
            }
            message += " on thread " + std::to_string(info.lastHit.threadId) + ", hit " +
                       std::to_string(info.hitCount) + ".";
            queueEvent(std::move(message), true);
        },
        [this](std::uint32_t exitCode) {
            queueEvent("The target process exited with code " + std::to_string(exitCode) + ".", false);
        });
}

void BreakpointService::detachDebugger() {
    // The pump restores every original byte on its way out.
    debugPump_.detach();
}

infra::Result<void> BreakpointService::addBreakpoint(std::uintptr_t address, std::string label,
                                                     domain::BreakpointKind kind, std::uint8_t length) {
    auto attach = attachDebugger();
    if (!attach) {
        return attach;
    }

    auto added = debugPump_.addBreakpoint(address, std::move(label), kind, length);
    if (added) {
        infra::Logger::instance().info(std::string(domain::breakpointKindName(kind)) + " breakpoint set at " +
                                       domain::toHex(address) + ".");
    }
    return added;
}

infra::Result<void> BreakpointService::removeBreakpoint(std::uintptr_t address) {
    auto removed = debugPump_.removeBreakpoint(address);
    if (removed) {
        infra::Logger::instance().info("Breakpoint removed at " + domain::toHex(address) + ".");
    } else {
        infra::Logger::instance().error(removed.error());
    }
    return removed;
}

std::vector<domain::BreakpointInfo> BreakpointService::breakpoints() const {
    return debugPump_.breakpoints();
}

bool BreakpointService::debuggerAttached() const {
    return debugPump_.attached();
}

std::vector<std::string> BreakpointService::takeEvents() {
    std::scoped_lock lock(mutex_);
    return std::move(events_);
}

void BreakpointService::queueEvent(std::string message, bool rateLimited) {
    std::scoped_lock lock(mutex_);

    if (rateLimited) {
        // A breakpoint inside a hot loop fires thousands of times a second.
        // Logging and queueing every one of them would cost more than the
        // breakpoint itself and bury everything else in the log.
        const auto now = std::chrono::steady_clock::now();
        if (lastEvent_.time_since_epoch().count() != 0 && now - lastEvent_ < std::chrono::milliseconds(500)) {
            return;
        }
        lastEvent_ = now;
    }

    infra::Logger::instance().info(message);
    events_.push_back(std::move(message));
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
