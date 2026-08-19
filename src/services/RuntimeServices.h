#pragma once

#include "domain/TargetSession.h"
#include "engine_asm/Assembler.h"
#include "engine_disasm/Disassembler.h"
#include "engine_inject/Injector.h"
#include "engine_pointer/PointerScanner.h"
#include "engine_scan/MemoryScanner.h"
#include "platform_win32/Win32Platform.h"
#include "scripting/LuaScanner.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ire::services {

class AddressListService {
public:
    explicit AddressListService(domain::TargetSession& session);
    ~AddressListService();

    std::uint64_t add(std::uintptr_t address, domain::ValueType type, std::string description, std::string group);
    // An entry backed by a pointer chain re-resolves its address as the target
    // runs, so it keeps tracking the value across a restart.
    std::uint64_t addChain(domain::PointerChain chain, domain::ValueType type, std::string description,
                           std::string group);
    bool remove(std::uint64_t id);
    bool setFrozen(std::uint64_t id, bool frozen);
    bool updateValue(std::uint64_t id, const std::vector<std::uint8_t>& bytes);
    void toggleHotkey(const std::string& hotkey);

    // Recomputes the address of every chain-backed entry. Runs automatically in
    // the background; exposed so attaching can refresh immediately.
    void resolvePointerChains();

private:
    void freezeLoop(std::stop_token token);

    // Twenty writes a second. Slower than this and a fast counter visibly
    // fights back between writes; much faster and the freeze loop spends its
    // time in WriteProcessMemory for no visible gain.
    static constexpr int freezeIntervalMs = 50;
    // Every 10th pass, so pointer chains re-resolve about twice a second.
    static constexpr int resolveEveryTicks = 10;

    domain::TargetSession& session_;
    std::jthread worker_;
};

// A thin front for DebugEventPump. The pump owns the breakpoint table because
// arming has to be interleaved with the single-step sequence that follows every
// hit; keeping a second copy here is how the two used to disagree.
class BreakpointService {
public:
    explicit BreakpointService(domain::TargetSession& session);
    ~BreakpointService();

    infra::Result<void> attachDebugger();
    void detachDebugger();
    infra::Result<void> addBreakpoint(std::uintptr_t address, std::string label);
    infra::Result<void> removeBreakpoint(std::uintptr_t address);
    [[nodiscard]] std::vector<domain::BreakpointInfo> breakpoints() const;
    [[nodiscard]] bool debuggerAttached() const;

    // Hits arrive on the pump thread, which must not touch the UI. The UI polls
    // this once a frame instead. Rate limited, because a breakpoint inside a hot
    // loop fires far faster than anyone can read; the hit counts in the
    // breakpoint table are the authoritative record.
    [[nodiscard]] std::vector<std::string> takeEvents();

private:
    void queueEvent(std::string message, bool rateLimited);

    domain::TargetSession& session_;
    platform_win32::DebugEventPump debugPump_;
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
    std::chrono::steady_clock::time_point lastEvent_{};
};

class RuntimeServices {
public:
    RuntimeServices();

    platform_win32::Win32Platform& platform() { return platform_; }
    domain::TargetSession& session() { return session_; }
    AddressListService& addressList() { return addressList_; }
    engine_scan::ScanJob& scanJob() { return scanJob_; }
    scripting::LuaScanJob& luaScanJob() { return luaScanJob_; }
    engine_pointer::PointerScanJob& pointerScanJob() { return pointerScanJob_; }
    engine_disasm::Disassembler& disassembler() { return disassembler_; }
    engine_asm::Assembler& assembler() { return assembler_; }
    engine_inject::Injector& injector() { return injector_; }
    BreakpointService& breakpoints() { return breakpoints_; }

private:
    platform_win32::Win32Platform platform_;
    domain::TargetSession session_;
    AddressListService addressList_;
    engine_scan::ScanJob scanJob_;
    scripting::LuaScanJob luaScanJob_;
    engine_pointer::PointerScanJob pointerScanJob_;
    engine_disasm::Disassembler disassembler_;
    engine_asm::Assembler assembler_;
    engine_inject::Injector injector_;
    BreakpointService breakpoints_;
};

} // namespace ire::services
