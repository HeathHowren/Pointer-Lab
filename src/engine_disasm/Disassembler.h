#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"

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

} // namespace ire::engine_disasm

