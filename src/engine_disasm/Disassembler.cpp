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

    // The machine mode has to match the target, not the host. Decoding 32-bit
    // code as long mode does not fail cleanly -- it silently produces plausible
    // but wrong instructions, because most byte sequences decode as *something*
    // in both modes, and a REX prefix in long mode is an INC/DEC in legacy.
    const bool legacy = session.bitness() == domain::Bitness::X86;
    const auto machineMode = legacy ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LONG_64;
    const auto stackWidth = legacy ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_64;

    ZydisDecoder decoder;
    ZydisFormatter formatter;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, machineMode, stackWidth)) ||
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

std::optional<domain::Instruction> precedingInstruction(const Disassembler& disassembler,
                                                        domain::TargetSession& session, std::uintptr_t address) {
    // 15 is the architectural maximum instruction length, so a lookback beyond
    // it can only add noise for the single instruction we want. 24 gives a
    // little more room for the *run* leading up to it, which is what the
    // all-valid check below actually judges.
    constexpr std::size_t maxLookback = 24;

    if (!session.attached() || address <= maxLookback) {
        return std::nullopt;
    }

    // Candidates, by the address the preceding instruction would start at, with
    // how many independent start positions agree on it.
    struct Candidate {
        domain::Instruction instruction;
        int votes{};
    };
    std::vector<Candidate> candidates;

    for (std::size_t back = maxLookback; back >= 1; --back) {
        const auto start = address - back;
        // At most `back` instructions can fit in `back` bytes, since the
        // shortest instruction is one byte.
        const auto listing = disassembler.disassemble(session, start, back);
        if (listing.empty()) {
            continue;
        }

        std::optional<domain::Instruction> landing;
        for (const auto& instruction : listing) {
            // An undecodable byte means this alignment is not real code, and a
            // zero-length entry would loop forever.
            if (!instruction.valid || instruction.bytes.empty()) {
                break;
            }
            const auto end = instruction.address + instruction.bytes.size();
            if (end == address) {
                landing = instruction;
                break;
            }
            if (end > address) {
                // Straddles the target: this start position is out of phase.
                break;
            }
        }
        if (!landing) {
            continue;
        }

        const auto existing = std::find_if(candidates.begin(), candidates.end(), [&landing](const Candidate& c) {
            return c.instruction.address == landing->address;
        });
        if (existing != candidates.end()) {
            existing->votes += 1;
        } else {
            candidates.push_back({*landing, 1});
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    // Take the boundary the most start positions agree on.
    //
    // Trying only the longest lookback and trusting it is not good enough: the
    // bytes before a function, or before whatever the trap landed in, need not
    // be code at all, and a run of them can decode cleanly and land on the
    // target by luck. What makes this tractable is that x86 is
    // self-synchronising -- decoders started at different offsets converge
    // within a few instructions -- so the *correct* boundary is the one most
    // independent alignments reach, while a coincidental one is reached by one
    // or two. Ties go to the longer instruction, since a longer encoding
    // agreed on by as many alignments is the less likely coincidence.
    const auto best = std::max_element(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                                                const Candidate& b) {
        if (a.votes != b.votes) {
            return a.votes < b.votes;
        }
        return a.instruction.bytes.size() < b.instruction.bytes.size();
    });
    return best->instruction;
}

} // namespace ire::engine_disasm
