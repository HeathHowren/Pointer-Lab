#pragma once

#include "domain/TargetSession.h"
#include "engine_asm/Assembler.h"
#include "engine_disasm/Disassembler.h"
#include "engine_inject/Injector.h"
#include "engine_pointer/PointerScanner.h"
#include "engine_scan/MemoryScanner.h"
#include "platform_win32/Win32Platform.h"
#include "scripting/LuaScanner.h"

#include <map>
#include <mutex>

namespace ire::services {

class AddressListService {
public:
    explicit AddressListService(domain::TargetSession& session);
    ~AddressListService();

    std::uint64_t add(std::uintptr_t address, domain::ValueType type, std::string description, std::string group);
    bool remove(std::uint64_t id);
    bool setFrozen(std::uint64_t id, bool frozen);
    bool updateValue(std::uint64_t id, const std::vector<std::uint8_t>& bytes);
    void toggleHotkey(const std::string& hotkey);

private:
    void freezeLoop(std::stop_token token);

    domain::TargetSession& session_;
    std::jthread worker_;
};

class BreakpointService {
public:
    explicit BreakpointService(domain::TargetSession& session);
    ~BreakpointService();

    infra::Result<void> attachDebugger();
    void detachDebugger();
    infra::Result<void> addBreakpoint(std::uintptr_t address, std::string label);
    infra::Result<void> removeBreakpoint(std::uintptr_t address);
    std::vector<domain::BreakpointInfo> breakpoints() const;
    bool debuggerAttached() const;

private:
    void onBreakpointHit(std::uintptr_t address);

    domain::TargetSession& session_;
    mutable std::mutex mutex_;
    platform_win32::DebugEventPump debugPump_;
    std::map<std::uintptr_t, domain::BreakpointInfo> breakpoints_;
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
