// Tests for the Keystone-backed assembler and the Zydis-backed disassembler.
//
// The assembler needs no target, so its golden vectors are plain unit tests.
// The disassembler reads through a TargetSession, so it is exercised against
// the helper process: bytes are assembled, written into the target, and read
// back out through the real Win32 path.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_asm/Assembler.h"
#include "engine_disasm/Disassembler.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes assembleAt(const std::string& source, std::uintptr_t base = 0x140001000) {
    const engine_asm::Assembler assembler;
    auto result = assembler.assemble(source, base);
    INFO("assembling: " << source);
    REQUIRE(result.has_value());
    return result.value();
}

// Zydis and Keystone agree on content but not always on spacing, so text
// comparisons go through this.
std::string squash(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
               text.end());
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------

TEST_CASE("The assembler encodes single instructions correctly", "[asm]") {
    CHECK(assembleAt("nop") == Bytes{0x90});
    CHECK(assembleAt("ret") == Bytes{0xC3});
    CHECK(assembleAt("int3") == Bytes{0xCC});
    CHECK(assembleAt("push rbp") == Bytes{0x55});
    CHECK(assembleAt("pop rbp") == Bytes{0x5D});
    CHECK(assembleAt("xor eax, eax") == Bytes{0x31, 0xC0});
    CHECK(assembleAt("mov rbp, rsp") == Bytes{0x48, 0x89, 0xE5});
}

// The old hand-rolled assembler understood nine mnemonics and rejected
// everything else, so anything real had to be entered as raw bytes.
TEST_CASE("The assembler handles instructions the old one could not", "[asm]") {
    CHECK_FALSE(assembleAt("movzx eax, byte ptr [rcx+0x10]").empty());
    CHECK_FALSE(assembleAt("lea rax, [rip+0x20]").empty());
    CHECK_FALSE(assembleAt("cmp dword ptr [rbx+0x2c], 0x64").empty());
    CHECK_FALSE(assembleAt("movss dword ptr [rdx], xmm0").empty());
    CHECK_FALSE(assembleAt("imul r8d, r9d, 0x1f").empty());
}

TEST_CASE("Multiple lines assemble into one buffer", "[asm]") {
    const auto bytes = assembleAt("push rbp\nmov rbp, rsp\npop rbp\nret");
    CHECK(bytes == Bytes{0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3});
}

TEST_CASE("Comments and blank lines are ignored", "[asm]") {
    const auto expected = Bytes{0x90, 0xC3};
    CHECK(assembleAt("nop ; this is a comment\n\n  ret   ; and so is this") == expected);
    CHECK(assembleAt("// leading comment\nnop\n  \nret // trailing") == expected);
}

// Relative branches depend on where the patch will live, which is the whole
// reason assemble() takes a base address.
TEST_CASE("Relative branches resolve against the base address", "[asm]") {
    // A nearby target takes the two-byte short form: rel8 = 0x1005 - (0x1000+2).
    CHECK(assembleAt("jmp 0x1005", 0x1000) == Bytes{0xEB, 0x03});
    // Too far for rel8, so this becomes the five-byte form with rel32 = 0xFFB.
    CHECK(assembleAt("jmp 0x2000", 0x1000) == Bytes{0xE9, 0xFB, 0x0F, 0x00, 0x00});
    // call has no short form in 64-bit mode: rel32 = 0x1005 - (0x1000+5) = 0.
    CHECK(assembleAt("call 0x1005", 0x1000) == Bytes{0xE8, 0x00, 0x00, 0x00, 0x00});

    // The same source at a different base has to encode a different displacement.
    const auto low = assembleAt("jmp 0x2000", 0x1000);
    const auto high = assembleAt("jmp 0x2000", 0x1100);
    CHECK(low.size() == high.size());
    CHECK(low != high);
}

TEST_CASE("Bad assembly reports an error instead of guessing", "[asm]") {
    const engine_asm::Assembler assembler;

    auto nonsense = assembler.assemble("this is not an instruction", 0x1000);
    CHECK_FALSE(nonsense.has_value());
    CHECK_FALSE(nonsense.error().empty());

    auto badOperand = assembler.assemble("mov rax, rbx, rcx", 0x1000);
    CHECK_FALSE(badOperand.has_value());

    // An empty buffer must not look like a successful zero-byte patch.
    CHECK_FALSE(assembler.assemble("", 0x1000).has_value());
    CHECK_FALSE(assembler.assemble("   \n\n ; only a comment\n", 0x1000).has_value());
}

// ---------------------------------------------------------------------------
// Disassembler
// ---------------------------------------------------------------------------

TEST_CASE("The disassembler decodes bytes written into a live process", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    const auto scratch = fixture.helper.scratch();
    const std::string source = "push rbp\n"
                               "mov rbp, rsp\n"
                               "sub rsp, 0x20\n"
                               "xor eax, eax\n"
                               "add rsp, 0x20\n"
                               "pop rbp\n"
                               "ret";
    const auto code = assembleAt(source, scratch);
    REQUIRE(fixture.session.writeBytes(scratch, code).has_value());

    const auto instructions = disassembler.disassemble(fixture.session, scratch, 7);
    REQUIRE(instructions.size() == 7);

    CHECK(squash(instructions[0].text) == "pushrbp");
    CHECK(squash(instructions[1].text) == "movrbp,rsp");
    CHECK(squash(instructions[3].text) == "xoreax,eax");
    CHECK(squash(instructions[6].text) == "ret");

    // Addresses and lengths have to chain exactly, which is what the old
    // hardcoded instruction lengths got wrong.
    std::uintptr_t expectedAddress = scratch;
    Bytes reconstructed;
    for (const auto& instruction : instructions) {
        CHECK(instruction.address == expectedAddress);
        CHECK(instruction.valid);
        CHECK_FALSE(instruction.bytes.empty());
        expectedAddress += instruction.bytes.size();
        reconstructed.insert(reconstructed.end(), instruction.bytes.begin(), instruction.bytes.end());
    }
    CHECK(reconstructed == code);
}

// The strongest check available: whatever the disassembler prints has to
// assemble back to the exact bytes it came from.
TEST_CASE("Disassembled text reassembles to the original bytes", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    const engine_asm::Assembler assembler;

    const auto scratch = fixture.helper.scratch();
    // The branch target has to be near the patch: a fixed low address would be
    // billions of bytes away and could not be encoded relative at all.
    const auto code = assembleAt("mov eax, 0x2a\n"
                                 "cmp eax, 0x2a\n"
                                 "jne " + domain::toHex(scratch + 0x20) + "\n"
                                 "add rsp, 0x28\n"
                                 "ret",
                                 scratch);
    REQUIRE(fixture.session.writeBytes(scratch, code).has_value());

    const auto instructions = disassembler.disassemble(fixture.session, scratch, 5);
    REQUIRE(instructions.size() == 5);

    for (const auto& instruction : instructions) {
        INFO("round-tripping: " << instruction.text);
        auto again = assembler.assemble(instruction.text, instruction.address);
        REQUIRE(again.has_value());
        CHECK(again.value() == instruction.bytes);
    }
}

TEST_CASE("Branch targets are computed so the UI can follow them", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    const auto scratch = fixture.helper.scratch();
    // jmp forward over a nop, then a nop, then ret.
    const auto code = assembleAt("jmp " + domain::toHex(scratch + 6) + "\nnop\nret", scratch);
    REQUIRE(fixture.session.writeBytes(scratch, code).has_value());

    const auto instructions = disassembler.disassemble(fixture.session, scratch, 3);
    REQUIRE(instructions.size() == 3);

    CHECK(instructions[0].branchTarget == scratch + 6);
    // A nop and a ret go nowhere.
    CHECK(instructions[1].branchTarget == 0);
    CHECK(instructions[2].branchTarget == 0);
}

TEST_CASE("Undecodable bytes are shown as data instead of desynchronising", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    const auto scratch = fixture.helper.scratch();
    // 0x06 is an invalid opcode in 64-bit mode; a nop follows it.
    const Bytes code{0x06, 0x90, 0xC3};
    REQUIRE(fixture.session.writeBytes(scratch, code).has_value());

    const auto instructions = disassembler.disassemble(fixture.session, scratch, 3);
    REQUIRE(instructions.size() == 3);

    CHECK_FALSE(instructions[0].valid);
    CHECK(instructions[0].bytes == Bytes{0x06});
    // Crucially, the listing resynchronises on the very next byte.
    CHECK(instructions[1].address == scratch + 1);
    CHECK(squash(instructions[1].text) == "nop");
    CHECK(squash(instructions[2].text) == "ret");
}

// Real compiled code, not something we assembled ourselves. Every x64 ntdll
// syscall stub begins "mov r10, rcx", and system DLLs share a base across
// processes in a session, so this address is valid in the helper too.
TEST_CASE("A real function disassembles correctly", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    REQUIRE(ntdll != nullptr);
    auto* ntClose = GetProcAddress(ntdll, "NtClose");
    REQUIRE(ntClose != nullptr);

    const auto address = reinterpret_cast<std::uintptr_t>(ntClose);
    const auto instructions = disassembler.disassemble(fixture.session, address, 12);
    REQUIRE(instructions.size() == 12);

    CHECK(squash(instructions[0].text) == "movr10,rcx");

    bool sawSyscall = false;
    std::uintptr_t expectedAddress = address;
    for (const auto& instruction : instructions) {
        CHECK(instruction.address == expectedAddress);
        CHECK(instruction.valid);
        expectedAddress += instruction.bytes.size();
        if (squash(instruction.text) == "syscall") {
            sawSyscall = true;
        }
    }
    CHECK(sawSyscall);
}

// A patch shorter than the code it replaces is the classic way to crash a
// target: execution resumes in the middle of what is left of an instruction.
TEST_CASE("A short patch is padded out to an instruction boundary", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    const auto scratch = fixture.helper.scratch();
    // Four instructions, 4 + 3 + 1 + 1 = 9 bytes.
    const auto original = assembleAt("add rsp, 0x28\nmov rbp, rsp\nnop\nret", scratch);
    REQUIRE(original.size() == 9);
    REQUIRE(fixture.session.writeBytes(scratch, original).has_value());

    SECTION("a one-byte patch fills out the four-byte instruction it lands in") {
        const auto padded = engine_disasm::padToInstructionBoundary(disassembler, fixture.session, scratch, Bytes{0x90});
        CHECK(padded == Bytes{0x90, 0x90, 0x90, 0x90});
    }
    SECTION("a patch spanning into a second instruction fills out that one too") {
        // Five bytes covers all of the 4-byte add and one byte of the 3-byte mov.
        const Bytes patch(5, 0x90);
        const auto padded = engine_disasm::padToInstructionBoundary(disassembler, fixture.session, scratch, patch);
        CHECK(padded.size() == 7);
        CHECK(std::all_of(padded.begin(), padded.end(), [](std::uint8_t b) { return b == 0x90; }));
    }
    SECTION("a patch already on a boundary is left alone") {
        const Bytes patch(4, 0x90);
        CHECK(engine_disasm::padToInstructionBoundary(disassembler, fixture.session, scratch, patch) == patch);

        const Bytes exact(7, 0xCC);
        CHECK(engine_disasm::padToInstructionBoundary(disassembler, fixture.session, scratch, exact) == exact);
    }
    SECTION("the padded bytes preserve the patch itself") {
        const auto code = assembleAt("xor eax, eax", scratch); // 2 bytes
        const auto padded = engine_disasm::padToInstructionBoundary(disassembler, fixture.session, scratch, code);
        REQUIRE(padded.size() == 4);
        CHECK(Bytes(padded.begin(), padded.begin() + 2) == code);
        CHECK(padded[2] == 0x90);
        CHECK(padded[3] == 0x90);
    }
    SECTION("an empty patch stays empty") {
        CHECK(engine_disasm::padToInstructionBoundary(disassembler, fixture.session, scratch, {}).empty());
    }
}

TEST_CASE("Disassembling unreadable memory yields nothing rather than garbage", "[disasm][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    CHECK(disassembler.disassemble(fixture.session, 0x10, 8).empty());
    CHECK(disassembler.disassemble(fixture.session, fixture.helper.scratch(), 0).empty());
}
