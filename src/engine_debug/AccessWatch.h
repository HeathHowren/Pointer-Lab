#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"
#include "engine_disasm/Disassembler.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ire::engine_debug {

// What one register held when the watched address was touched, and what that
// value appears to be.
//
// The interpretation is the point. A bare "EDI = 0x1A2B0000" says nothing; "EDI
// = 0x1A2B0000, watched address is EDI+0xF8" tells the reader they have found
// the base of the structure the value lives in, which is the single most useful
// thing this whole feature produces.
struct RegisterMeaning {
    std::string name;
    std::uint64_t value{};
    std::string interpretation;
};

// One instruction observed touching the watched address.
struct AccessSite {
    // The instruction that performed the access. For a data watchpoint this is
    // walked back from the trap address; see instructionResolved.
    std::uintptr_t address{};
    std::string text;
    std::vector<std::uint8_t> bytes;
    std::uint64_t hitCount{};
    domain::RegisterContext lastContext;
    // Where the CPU actually reported the trap. For a data watchpoint that is
    // the instruction *after* the access, kept so the UI can show its working.
    std::uintptr_t trapAddress{};
    // False when the accessing instruction could not be identified by walking
    // back, in which case `address` is the trap address and the UI says so.
    // Reporting the wrong instruction confidently would send the reader to
    // patch code that never touched the value.
    bool instructionResolved{};
};

// "Find out what accesses this address."
//
// The mechanism is a hardware data breakpoint, which Pointer Lab already had.
// What this adds is the part that makes it usable: every hit is aggregated by
// the instruction responsible, rather than arriving as a flood of individual
// notifications that scroll past faster than anyone can read.
//
// Aggregation also solves the cost problem. Resolving an instruction means
// reading and disassembling target memory, which is far too expensive to do per
// hit inside a hot loop -- so it is done once per distinct instruction and the
// result cached. A breakpoint that fires ten thousand times in a game's render
// loop resolves two or three sites and then costs a map lookup per hit.
class AccessWatch {
public:
    AccessWatch(domain::TargetSession& session, const engine_disasm::Disassembler& disassembler);

    // Records what is being watched, so results can be labelled and offsets
    // from the watched address computed. Clears any previous results.
    void begin(std::uintptr_t address, std::uint8_t length, domain::BreakpointKind kind);
    void stop();

    [[nodiscard]] bool active() const;
    [[nodiscard]] std::uintptr_t watchedAddress() const;
    [[nodiscard]] std::uint8_t watchedLength() const;
    [[nodiscard]] domain::BreakpointKind kind() const;

    // Called from the debug pump thread on every hit. Cheap after the first hit
    // at a given instruction.
    void record(const domain::RegisterContext& context);

    [[nodiscard]] std::vector<AccessSite> sites() const;
    [[nodiscard]] std::uint64_t totalHits() const;
    // True once the site cap was reached and further distinct instructions were
    // dropped. Surfaced rather than hidden, so a partial list is never mistaken
    // for a complete one.
    [[nodiscard]] bool truncated() const;
    void clear();

    // Explains each register of a captured context against the current target:
    // where it points, and its offset from the watched address when it is
    // plausibly the base of the structure containing it.
    [[nodiscard]] std::vector<RegisterMeaning> explain(const domain::RegisterContext& context) const;

    // More than this many distinct instructions touching one address means the
    // watch is on something far too general to be useful -- a shared allocator
    // header, say. Capping bounds memory and says so rather than growing until
    // the UI stops responding.
    static constexpr std::size_t maxSites = 256;

    // How far above the watched address a register can point and still be
    // reported as a probable structure base. Game structures are routinely a
    // few hundred bytes; beyond a page it is far more likely to be coincidence.
    static constexpr std::uint64_t maxStructOffset = 0x1000;

private:
    domain::TargetSession& session_;
    const engine_disasm::Disassembler& disassembler_;

    mutable std::mutex mutex_;
    bool active_{};
    std::uintptr_t watched_{};
    std::uint8_t length_{1};
    domain::BreakpointKind kind_{domain::BreakpointKind::HardwareWrite};
    // Keyed by trap address, not by resolved instruction address: the trap
    // address is what arrives on every hit, so this is the lookup that has to be
    // fast.
    std::unordered_map<std::uintptr_t, AccessSite> sites_;
    std::uint64_t totalHits_{};
    bool truncated_{};
};

} // namespace ire::engine_debug
