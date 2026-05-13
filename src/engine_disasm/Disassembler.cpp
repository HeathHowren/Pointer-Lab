#include "engine_disasm/Disassembler.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace ire::engine_disasm {

namespace {

std::int32_t rel32(const std::vector<std::uint8_t>& bytes, std::size_t index) {
    std::int32_t value{};
    std::memcpy(&value, bytes.data() + index, sizeof(value));
    return value;
}

std::int8_t rel8(const std::vector<std::uint8_t>& bytes, std::size_t index) {
    std::int8_t value{};
    std::memcpy(&value, bytes.data() + index, sizeof(value));
    return value;
}

std::uint64_t imm64(const std::vector<std::uint8_t>& bytes, std::size_t index) {
    std::uint64_t value{};
    std::memcpy(&value, bytes.data() + index, sizeof(value));
    return value;
}

domain::Instruction decodeOne(std::uintptr_t address, const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto op = bytes[offset];
    domain::Instruction ins;
    ins.address = address + offset;
    auto take = [&](std::size_t n, std::string text) {
        const auto end = std::min(bytes.size(), offset + n);
        ins.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        ins.text = std::move(text);
        return ins;
    };

    switch (op) {
    case 0x90: return take(1, "nop");
    case 0xC3: return take(1, "ret");
    case 0xCC: return take(1, "int3");
    case 0x55: return take(1, "push rbp");
    case 0x53: return take(1, "push rbx");
    case 0x57: return take(1, "push rdi");
    case 0x56: return take(1, "push rsi");
    case 0x5D: return take(1, "pop rbp");
    case 0x5B: return take(1, "pop rbx");
    case 0x5F: return take(1, "pop rdi");
    case 0x5E: return take(1, "pop rsi");
    case 0x31:
        if (offset + 1 < bytes.size() && bytes[offset + 1] == 0xC0) return take(2, "xor eax, eax");
        break;
    case 0x33:
        if (offset + 1 < bytes.size() && bytes[offset + 1] == 0xC0) return take(2, "xor eax, eax");
        break;
    case 0x48:
        if (offset + 1 < bytes.size() && bytes[offset + 1] == 0x89) return take(3, "mov r/m64, r64");
        if (offset + 1 < bytes.size() && bytes[offset + 1] == 0x8B) return take(3, "mov r64, r/m64");
        if (offset + 9 < bytes.size() && bytes[offset + 1] == 0xB8) {
            return take(10, "mov rax, " + domain::toHex(static_cast<std::uintptr_t>(imm64(bytes, offset + 2))));
        }
        break;
    case 0x8B: return take(std::min<std::size_t>(3, bytes.size() - offset), "mov r32, r/m32");
    case 0x89: return take(std::min<std::size_t>(3, bytes.size() - offset), "mov r/m32, r32");
    case 0xE8:
        if (offset + 4 < bytes.size()) return take(5, "call " + domain::toHex(address + offset + 5 + rel32(bytes, offset + 1)));
        break;
    case 0xE9:
        if (offset + 4 < bytes.size()) return take(5, "jmp " + domain::toHex(address + offset + 5 + rel32(bytes, offset + 1)));
        break;
    case 0xEB:
        if (offset + 1 < bytes.size()) return take(2, "jmp " + domain::toHex(address + offset + 2 + rel8(bytes, offset + 1)));
        break;
    case 0x74:
        if (offset + 1 < bytes.size()) return take(2, "je " + domain::toHex(address + offset + 2 + rel8(bytes, offset + 1)));
        break;
    case 0x75:
        if (offset + 1 < bytes.size()) return take(2, "jne " + domain::toHex(address + offset + 2 + rel8(bytes, offset + 1)));
        break;
    default:
        break;
    }

    return take(1, "db 0x" + domain::bytesToHex({op}, false));
}

} // namespace

std::vector<domain::Instruction> Disassembler::disassemble(domain::TargetSession& session, std::uintptr_t address, std::size_t instructionCount) const {
    std::vector<domain::Instruction> instructions;
    auto bytes = session.readBytes(address, 512);
    if (!bytes) {
        return instructions;
    }

    std::size_t offset{};
    while (offset < bytes.value().size() && instructions.size() < instructionCount) {
        auto ins = decodeOne(address, bytes.value(), offset);
        offset += std::max<std::size_t>(1, ins.bytes.size());
        instructions.push_back(std::move(ins));
    }
    return instructions;
}

} // namespace ire::engine_disasm
