#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"

#include <optional>
#include <vector>

namespace ire::engine_disasm {

class Disassembler {
public:
    // Decodes up to instructionCount instructions starting at address. Bytes
    // that do not decode come back as single-byte entries with valid == false,
    // so the listing stays aligned instead of drifting.
    std::vector<domain::Instruction> disassemble(domain::TargetSession& session, std::uintptr_t address, std::size_t instructionCount) const;
};

// Appends nops so patch covers whole instructions starting at address. A patch
// shorter than the code it overwrites otherwise leaves a truncated instruction
// behind, and the target crashes the moment execution reaches it.
[[nodiscard]] std::vector<std::uint8_t> padToInstructionBoundary(const Disassembler& disassembler,
                                                                 domain::TargetSession& session,
                                                                 std::uintptr_t address,
                                                                 std::vector<std::uint8_t> patch);

// The instruction that ends exactly at `address`, if one can be identified.
//
// x86 cannot be disassembled backwards: instructions are 1 to 15 bytes long and
// there is no way to know where the previous one began without already knowing.
// This matters because a *data* breakpoint traps after the access completes, so
// the reported instruction pointer names the instruction after the one that
// touched the address -- and the one that touched it is the whole answer the
// user asked for.
//
// The approach relies on x86 being self-synchronising: decoders started at
// different offsets converge on the same boundaries within a few instructions.
// So every start position from 24 bytes back is decoded independently, each one
// votes for the instruction it finds ending at `address`, and the boundary with
// the most votes wins. A coincidental alignment is reached by one or two start
// positions; the real one is reached by most of them.
//
// Returns nullopt when nothing lands there at all, which is honest rather than
// unhelpful: a confident wrong answer here sends the reader to patch the wrong
// instruction.
[[nodiscard]] std::optional<domain::Instruction> precedingInstruction(const Disassembler& disassembler,
                                                                      domain::TargetSession& session,
                                                                      std::uintptr_t address);

} // namespace ire::engine_disasm

