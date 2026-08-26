#pragma once

#include "domain/Domain.h"
#include "infra/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ire::engine_asm {

// x86 Intel-syntax assembler, backed by Keystone. Assembles for either bitness.
class Assembler {
public:
    // Assembles source as if it were placed at baseAddress. The address is not
    // decoration: a relative jmp or call encodes a different displacement
    // depending on where the patch will actually live, so assembling at the
    // wrong base produces bytes that branch somewhere else entirely.
    //
    // bitness must match the target. Assembling "push eax" in 64-bit mode is a
    // hard error, but the dangerous direction is quieter: several instructions
    // assemble in both modes to different bytes, so a 64-bit encoding written
    // into a 32-bit process is accepted by the assembler and crashes the target.
    //
    // One instruction per line; ';' and '//' comments and blank lines are
    // stripped first. Keystone chooses the shortest valid encoding, so a short
    // jump comes back as two bytes rather than five. Failure carries Keystone's
    // own message and error code.
    infra::Result<std::vector<std::uint8_t>> assemble(const std::string& source, std::uintptr_t baseAddress,
                                                      domain::Bitness bitness = domain::Bitness::X64) const;
};

} // namespace ire::engine_asm

