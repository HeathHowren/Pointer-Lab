#pragma once

#include "infra/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ire::engine_asm {

class Assembler {
public:
    infra::Result<std::vector<std::uint8_t>> assemble(const std::string& source, std::uintptr_t baseAddress) const;
};

} // namespace ire::engine_asm

