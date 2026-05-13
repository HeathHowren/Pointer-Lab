#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"

#include <vector>

namespace ire::engine_disasm {

class Disassembler {
public:
    std::vector<domain::Instruction> disassemble(domain::TargetSession& session, std::uintptr_t address, std::size_t instructionCount) const;
};

} // namespace ire::engine_disasm

