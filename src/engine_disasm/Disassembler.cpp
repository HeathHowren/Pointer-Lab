#include "engine_disasm/Disassembler.h"

#include <Zydis/Zydis.h>

#include <algorithm>

namespace ire::engine_disasm {

namespace {

// A whole listing is never worth more than this in one read, however many
// instructions were asked for.
constexpr std::size_t maxReadSize = 64 * 1024;

std::uintptr_t branchTarget(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands,
                            ZyanU64 runtimeAddress) {
    switch (instruction.meta.category) {
    case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_COND_BR:
    case ZYDIS_CATEGORY_UNCOND_BR:
        break;
    default:
        return 0;
    }

    for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i) {
        // Only a relative immediate has a destination we can compute here; an
        // indirect branch through a register is unknowable without the context.
        if (operands[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operands[i].imm.is_relative) {
            continue;
        }
        ZyanU64 absolute{};
        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[i], runtimeAddress, &absolute))) {
            return static_cast<std::uintptr_t>(absolute);
        }
    }
    return 0;
}

} // namespace

std::vector<domain::Instruction> Disassembler::disassemble(domain::TargetSession& session, std::uintptr_t address,
                                                           std::size_t instructionCount) const {
    std::vector<domain::Instruction> instructions;
    if (instructionCount == 0) {
        return instructions;
    }

    // 15 bytes is the architectural maximum instruction length, so this is
    // always enough for the requested count without a second read. The old
    // fixed 512-byte read silently produced a short listing for large counts.
    const std::size_t wanted =
        std::min(maxReadSize, instructionCount * static_cast<std::size_t>(ZYDIS_MAX_INSTRUCTION_LENGTH));

    // A read that crosses into an unmapped page comes back short rather than
    // failing, which is exactly what we want at the end of a module.
    auto bytes = session.readBytes(address, wanted);
    if (!bytes || bytes.value().empty()) {
        return instructions;
    }
    const auto& buffer = bytes.value();

    ZydisDecoder decoder;
    ZydisFormatter formatter;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)) ||
        !ZYAN_SUCCESS(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL))) {
        return instructions;
    }

    instructions.reserve(instructionCount);
    std::size_t offset{};
    while (offset < buffer.size() && instructions.size() < instructionCount) {
        const auto runtimeAddress = static_cast<ZyanU64>(address + offset);
        const auto begin = buffer.begin() + static_cast<std::ptrdiff_t>(offset);

        domain::Instruction result;
        result.address = address + offset;

        ZydisDecodedInstruction decoded{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, buffer.data() + offset, buffer.size() - offset, &decoded,
                                                operands))) {
            char text[256]{};
            if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &decoded, operands,
                                                             decoded.operand_count_visible, text, sizeof(text),
                                                             runtimeAddress, nullptr))) {
                result.text = text;
            } else {
                result.text = ZydisMnemonicGetString(decoded.mnemonic);
            }
            result.bytes.assign(begin, begin + decoded.length);
            result.branchTarget = branchTarget(decoded, operands, runtimeAddress);
            offset += decoded.length;
        } else {
            // Undecodable bytes are shown as data and we resynchronise one byte
            // later, which is what a real listing does past the end of a
            // function. Advancing by a guessed length instead is what used to
            // desynchronise the whole listing.
            result.bytes.assign(1, buffer[offset]);
            result.text = "db 0x" + domain::bytesToHex(result.bytes, false);
            result.valid = false;
            offset += 1;
        }

        instructions.push_back(std::move(result));
    }

    return instructions;
}

std::vector<std::uint8_t> padToInstructionBoundary(const Disassembler& disassembler, domain::TargetSession& session,
                                                   std::uintptr_t address, std::vector<std::uint8_t> patch) {
    if (patch.empty() || !session.attached()) {
        return patch;
    }

    // Work out where the last instruction the patch overlaps actually ends. 32
    // is far more than any patch can straddle: the shortest instruction is one
    // byte, so a patch long enough to need more would be pathological.
    const auto covering = disassembler.disassemble(session, address, 32);
    std::size_t covered{};
    for (const auto& instruction : covering) {
        if (covered >= patch.size()) {
            break;
        }
        // A zero-length entry would spin without making progress; the decoder
        // never produces one, but the loop must not depend on that.
        if (instruction.bytes.empty()) {
            return patch;
        }
        covered += instruction.bytes.size();
    }

    if (covered > patch.size()) {
        patch.resize(covered, 0x90);
    }
    return patch;
}

} // namespace ire::engine_disasm
