// Tests for attaching to a 32-bit (WOW64) target from this 64-bit build.
//
// These exist because every one of them failed before WOW64 support, and each
// failed *quietly*: the scan completed and found nothing, the chain "broke" at
// step one, the disassembly listed plausible instructions that were not the
// ones in the process. None of it raised an error, which is precisely why it
// needs tests rather than a manual check.
//
// The fixture is tests/helper/main.cpp compiled for Win32. It is optional, so
// every case here skips rather than fails when it was not built.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "domain/TargetSession.h"
#include "engine_asm/Assembler.h"
#include "engine_disasm/Disassembler.h"
#include "engine_pointer/PointerScanner.h"
#include "engine_scan/MemoryScanner.h"
#include "engine_symbols/ExportResolver.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

using namespace ire;
using testsupport::HelperBitness;

namespace {

// Every case starts with this. Catch2's SKIP marks the test as skipped rather
// than passed, so an absent fixture cannot be mistaken for coverage.
void requireHelper32() {
    if (!testsupport::helperAvailable(HelperBitness::X86)) {
        SKIP("The 32-bit test helper was not built (-DPOINTERLAB_BUILD_HELPER32=OFF).");
    }
}

// Waits for a scan job to finish, so the assertions do not race the worker.
bool waitForScan(engine_scan::ScanJob& job, std::chrono::seconds timeout = std::chrono::seconds(120)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!job.progress().running) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForPointerScan(engine_pointer::PointerScanJob& job,
                        std::chrono::seconds timeout = std::chrono::seconds(180)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!job.progress().running) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

engine_scan::ScanOptions testOptions() {
    engine_scan::ScanOptions options;
    options.writableOnly = true;
    options.maxResults = 20000000;
    return options;
}

} // namespace

TEST_CASE("A 32-bit target is recognised as 32-bit", "[wow64][integration]") {
    requireHelper32();
    testsupport::AttachedHelper fixture(HelperBitness::X86);

    CHECK(fixture.session.bitness() == domain::Bitness::X86);
    CHECK(fixture.session.pointerSize() == 4);

    // Every 32-bit address is below 4 GB by construction. A module base above
    // it would mean we had enumerated the 64-bit side of the WOW64 process.
    const auto modules = fixture.session.modules();
    REQUIRE_FALSE(modules.empty());
    const auto image = std::find_if(modules.begin(), modules.end(), [](const domain::ModuleInfo& module) {
        return module.name.find(L"pointerlab_test_helper32") != std::wstring::npos;
    });
    REQUIRE(image != modules.end());
    CHECK(image->base < 0x100000000ULL);
}

TEST_CASE("A 64-bit target is still recognised as 64-bit", "[wow64][integration]") {
    // The control. A change that made everything look 32-bit would otherwise
    // pass every test in this file.
    testsupport::AttachedHelper fixture(HelperBitness::X64);
    CHECK(fixture.session.bitness() == domain::Bitness::X64);
    CHECK(fixture.session.pointerSize() == 8);
}

TEST_CASE("Scanning finds a known value in a 32-bit target", "[wow64][integration]") {
    requireHelper32();
    testsupport::AttachedHelper fixture(HelperBitness::X86);

    // Value scanning is bitness-agnostic -- an i32 is four bytes either way --
    // so this is the baseline the pointer tests build on. If it fails, nothing
    // below it means anything.
    engine_scan::ScanJob job(fixture.session, testOptions());
    const auto needle = domain::parseScanValue(domain::ValueType::Int32, std::to_string(testsupport::needleValue));
    REQUIRE(needle.has_value());
    job.startFirst(domain::ScanMode::Exact, *needle);
    REQUIRE(waitForScan(job));

    const auto results = job.results();
    CHECK_FALSE(results.empty());
    CHECK(std::any_of(results.begin(), results.end(),
                      [&fixture](const domain::ScanResult& result) {
                          return result.address == fixture.helper.address();
                      }));
}

TEST_CASE("A pointer chain resolves in a 32-bit target", "[wow64][integration]") {
    requireHelper32();
    testsupport::AttachedHelper fixture(HelperBitness::X86);

    // This is the case that motivated the whole change. resolveChain used to
    // read sizeof(std::uintptr_t) -- 8 bytes -- so the first hop produced an
    // address made of two adjacent 32-bit pointers glued together, and every
    // chain reported "the chain broke" at step one.
    engine_pointer::PointerScanJob job(fixture.session);
    engine_pointer::PointerScanOptions options;
    options.target = fixture.helper.address();
    options.maxDepth = 3;
    options.maxOffset = 0x400;
    job.start(options);
    REQUIRE(waitForPointerScan(job));

    const auto chains = job.results();
    REQUIRE_FALSE(chains.empty());

    // At least one discovered chain must actually resolve back to the value.
    const auto resolves = std::any_of(chains.begin(), chains.end(), [&fixture](const domain::PointerChain& chain) {
        auto resolved = engine_pointer::resolveChain(fixture.session, chain);
        return resolved && resolved.value() == fixture.helper.address();
    });
    CHECK(resolves);
}

TEST_CASE("Pointer reads use the target's width, not the host's", "[wow64][integration]") {
    requireHelper32();
    testsupport::AttachedHelper fixture(HelperBitness::X86);

    // The helper's root global holds a pointer to its Node. Reading it as 8
    // bytes would splice in whatever follows it in memory.
    const auto pointer = fixture.session.readPointer(fixture.helper.root());
    REQUIRE(pointer.has_value());
    CHECK(pointer.value() != 0);
    CHECK(pointer.value() < 0x100000000ULL);

    // And the same address read as raw bytes must agree with it.
    auto raw = fixture.session.readBytes(fixture.helper.root(), 4);
    REQUIRE(raw.has_value());
    REQUIRE(raw.value().size() == 4);
    std::uint32_t expected{};
    std::memcpy(&expected, raw.value().data(), sizeof(expected));
    CHECK(pointer.value() == static_cast<std::uintptr_t>(expected));
}

TEST_CASE("The disassembler decodes 32-bit code in legacy mode", "[wow64][integration]") {
    requireHelper32();
    testsupport::AttachedHelper fixture(HelperBitness::X86);

    const engine_asm::Assembler assembler;
    const engine_disasm::Disassembler disassembler;

    // "push eax" does not exist in long mode -- 0x50 there is "push rax" -- so
    // a listing that prints it proves the decoder is genuinely in legacy mode
    // rather than defaulting to 64-bit and happening to agree.
    auto encoded = assembler.assemble("push eax\npop eax\nnop", 0, domain::Bitness::X86);
    REQUIRE(encoded.has_value());

    const auto scratch = fixture.helper.scratch();
    REQUIRE(fixture.session.writeBytes(scratch, encoded.value()).has_value());

    const auto instructions = disassembler.disassemble(fixture.session, scratch, 3);
    REQUIRE(instructions.size() == 3);
    CHECK(instructions[0].valid);
    CHECK(instructions[0].text.find("eax") != std::string::npos);
    CHECK(instructions[2].text.find("nop") != std::string::npos);
}

TEST_CASE("The assembler refuses 64-bit registers for a 32-bit target", "[wow64]") {
    const engine_asm::Assembler assembler;

    // Not pedantry: assembling "mov rax, 1" for a 32-bit process and writing
    // the result would corrupt the instruction stream. Keystone catches it only
    // because it was told which mode to use.
    CHECK_FALSE(assembler.assemble("mov rax, 1", 0x1000, domain::Bitness::X86).has_value());
    CHECK(assembler.assemble("mov eax, 1", 0x1000, domain::Bitness::X86).has_value());
    CHECK(assembler.assemble("mov rax, 1", 0x1000, domain::Bitness::X64).has_value());
}

TEST_CASE("The same source assembles differently per bitness", "[wow64]") {
    const engine_asm::Assembler assembler;

    // The quiet failure mode. "inc eax" is one byte (0x40) in 32-bit and two
    // (FF C0) in 64-bit, because 0x40 became the REX prefix. Both assemble
    // without complaint; only one is correct for a given target.
    auto asX86 = assembler.assemble("inc eax", 0x1000, domain::Bitness::X86);
    auto asX64 = assembler.assemble("inc eax", 0x1000, domain::Bitness::X64);
    REQUIRE(asX86.has_value());
    REQUIRE(asX64.has_value());
    CHECK(asX86.value() != asX64.value());
}

TEST_CASE("Exports resolve out of a 32-bit target's own kernel32", "[wow64][integration]") {
    requireHelper32();
    testsupport::AttachedHelper fixture(HelperBitness::X86);

    const engine_symbols::ExportResolver resolver;
    auto loadLibrary = resolver.resolve(fixture.session, L"kernel32.dll", "LoadLibraryW");
    REQUIRE(loadLibrary.has_value());

    // The address must be inside the *target's* 32-bit address space, and must
    // not be this process's own LoadLibraryW -- which is the bug this replaced.
    CHECK(loadLibrary.value() != 0);
    CHECK(loadLibrary.value() < 0x100000000ULL);

    const auto ours = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    CHECK(loadLibrary.value() != ours);
}

TEST_CASE("Export resolution follows forwarders", "[wow64][integration]") {
    // Against a 64-bit target, so it runs even without the optional fixture.
    testsupport::AttachedHelper fixture(HelperBitness::X64);

    const engine_symbols::ExportResolver resolver;
    auto loadLibrary = resolver.resolve(fixture.session, L"kernel32.dll", "LoadLibraryW");
    REQUIRE(loadLibrary.has_value());

    // On every modern Windows, kernel32's LoadLibraryW forwards to KERNELBASE.
    // A resolver that ignored forwarders would return an address inside
    // kernel32's export directory -- i.e. inside a string table. Whatever it
    // returns must at least be executable code, so it cannot be there.
    const auto base = engine_symbols::ExportResolver::moduleBase(fixture.session, L"kernel32.dll");
    REQUIRE(base != 0);

    // Same process, same DLL, so our own GetProcAddress is the ground truth
    // here -- it follows forwarders too.
    const auto ours = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    CHECK(loadLibrary.value() == ours);
}

TEST_CASE("An unknown export and an unloaded module fail with a clear message", "[wow64][integration]") {
    testsupport::AttachedHelper fixture(HelperBitness::X64);
    const engine_symbols::ExportResolver resolver;

    auto missing = resolver.resolve(fixture.session, L"kernel32.dll", "ThisExportDoesNotExist");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("not exported") != std::string::npos);

    auto absent = resolver.resolve(fixture.session, L"definitely_not_loaded.dll", "Anything");
    REQUIRE_FALSE(absent.has_value());
    CHECK(absent.error().find("not loaded") != std::string::npos);
}
