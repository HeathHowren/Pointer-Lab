// Tests for the auto-assembler.
//
// A script's promise is that it can be taken back: [ENABLE] patches the target,
// [DISABLE] puts it back, and nothing in between is left half-done. So most of
// these are round-trips against a real process -- write, assert the bytes
// changed, disable, assert they are byte-for-byte what they were.
//
// The refusals get as much attention as the successes, and deliberately so. An
// assembler that quietly takes the first of two pattern matches, or writes half
// a script and then gives up, produces a target that is broken in a way nobody
// can see. Every one of those cases is a test here.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_aa/AutoAssembler.h"

#include <algorithm>
#include <cstdlib>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

// The module the helper's own globals live in, and therefore the one
// aobscanmodule is pointed at.
constexpr const char* helperModule = "pointerlab_test_helper.exe";

// Eight bytes that will not occur by accident anywhere in the image. Written
// over the helper's `g_root`, which nothing in the helper ever dereferences, so
// a scan has something unique to find and a patch has something safe to cover.
const std::vector<std::uint8_t> marker{0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};

// Everything an AutoAssembler borrows, in one place. Declaration order is the
// construction order the assembler's references require.
struct Fixture {
    AttachedHelper attached;
    engine_asm::Assembler assembler;
    engine_patch::PatchRegistry patches{attached.session};
    engine_symbols::SymbolTable symbols;
    engine_inject::Injector injector{attached.session};
    engine_aa::AutoAssembler aa{attached.session, assembler, patches, symbols, injector};

    [[nodiscard]] domain::TargetSession& session() { return attached.session; }
    [[nodiscard]] std::uintptr_t markerAddress() const { return attached.helper.root(); }

    std::vector<std::uint8_t> read(std::uintptr_t address, std::size_t size) {
        auto bytes = attached.session.readBytes(address, size);
        REQUIRE(bytes.has_value());
        return bytes.value();
    }
};

// Plants the marker and puts the original bytes back when the test ends,
// however it ends. The helper keeps running throughout, so leaving its data
// section scribbled on would leak into whatever runs next.
class PlantedMarker {
public:
    explicit PlantedMarker(Fixture& fixture) : fixture_(fixture), address_(fixture.markerAddress()) {
        original_ = fixture_.read(address_, marker.size());
        REQUIRE(fixture_.session().writeBytes(address_, marker).has_value());
    }
    ~PlantedMarker() {
        static_cast<void>(fixture_.session().writeBytes(address_, original_));
    }

    PlantedMarker(const PlantedMarker&) = delete;
    PlantedMarker& operator=(const PlantedMarker&) = delete;

    [[nodiscard]] std::uintptr_t address() const { return address_; }

private:
    Fixture& fixture_;
    std::uintptr_t address_{};
    std::vector<std::uint8_t> original_;
};

bool mentions(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

std::int32_t readRel32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return static_cast<std::int32_t>(value);
}

} // namespace

TEST_CASE("a script allocates, writes data into it and frees it again", "[aa][integration]") {
    Fixture fixture;

    const std::string source =
        "[ENABLE]\n"
        "alloc(store, 0x40)\n"
        "registersymbol(store)\n"
        "store:\n"
        "  db 01 02\n"
        "  dw 0304\n"
        "  dd 05060708\n"
        "  dq 090A0B0C0D0E0F10\n"
        "[DISABLE]\n"
        "unregistersymbol(store)\n"
        "dealloc(store)\n";

    const auto id = fixture.aa.add("data", source);
    REQUIRE(fixture.aa.setEnabled(id, true).has_value());

    // registersymbol is what makes the allocation reachable from outside the
    // script at all, so this is two assertions in one.
    const auto store = fixture.symbols.resolve(fixture.session(), "store");
    REQUIRE(store.has_value());

    // Little-endian in each directive's own width, which is the convention
    // every other byte display in the tool uses.
    const std::vector<std::uint8_t> expected{0x01, 0x02, 0x04, 0x03, 0x08, 0x07, 0x06, 0x05,
                                             0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09};
    CHECK(fixture.read(store.value(), expected.size()) == expected);

    REQUIRE(fixture.aa.setEnabled(id, false).has_value());
    CHECK_FALSE(fixture.symbols.find("store").has_value());
    // Freed, so the page is no longer mapped and the read fails rather than
    // returning stale bytes.
    CHECK_FALSE(fixture.session().readBytes(store.value(), 4).has_value());
}

TEST_CASE("an injection finds its own address, allocates within reach and jumps there",
          "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);
    const auto inject = planted.address();

    const std::string source =
        std::string("[ENABLE]\n") +
        "aobscanmodule(INJECT, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "alloc(newmem, 0x100, INJECT)\n"
        "registersymbol(newmem)\n"
        "label(retpoint)\n"
        "\n"
        "newmem:\n"
        "  nop\n"
        "  jmp retpoint\n"
        "\n"
        "INJECT:\n"
        "  jmp newmem\n"
        "retpoint:\n"
        "\n"
        "[DISABLE]\n"
        "unregistersymbol(newmem)\n"
        "dealloc(newmem)\n";

    const auto id = fixture.aa.add("injection", source);
    REQUIRE(fixture.aa.setEnabled(id, true).has_value());

    const auto newmem = fixture.symbols.resolve(fixture.session(), "newmem");
    REQUIRE(newmem.has_value());

    // The reason alloc takes a third argument. A five-byte jmp encodes a signed
    // 32-bit displacement, so a cave further away than this could not be
    // reached at all -- the script would not assemble rather than misbehave,
    // but it still would not work.
    const auto distance = newmem.value() > inject ? newmem.value() - inject : inject - newmem.value();
    CHECK(distance < 0x80000000ULL);

    const auto patched = fixture.read(inject, marker.size());
    CHECK(patched[0] == 0xE9);
    CHECK(readRel32(patched, 1) == static_cast<std::int32_t>(newmem.value() - (inject + 5)));
    // Only the five bytes the jmp needs were touched. A patch that rounded up
    // to the whole pattern would be destroying bytes it had no reason to.
    CHECK(std::vector<std::uint8_t>(patched.begin() + 5, patched.end()) ==
          std::vector<std::uint8_t>(marker.begin() + 5, marker.end()));

    // And the other half of the layout: the jump back is computed from where
    // the cave actually landed, which is not known until after the allocation.
    const auto cave = fixture.read(newmem.value(), 6);
    CHECK(cave[0] == 0x90);
    REQUIRE(cave[1] == 0xE9);
    CHECK(newmem.value() + 6 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(readRel32(cave, 2))) ==
          inject + 5);

    REQUIRE(fixture.aa.setEnabled(id, false).has_value());
    CHECK(fixture.read(inject, marker.size()) == marker);
    CHECK_FALSE(fixture.symbols.find("newmem").has_value());
    CHECK_FALSE(fixture.session().readBytes(newmem.value(), 4).has_value());
}

TEST_CASE("check compiles a script without writing or allocating anything", "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);

    const std::string source =
        std::string("[ENABLE]\n") +
        "aobscanmodule(INJECT, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "alloc(newmem, 0x100, INJECT)\n"
        "newmem:\n"
        "  nop\n"
        "INJECT:\n"
        "  jmp newmem\n";

    auto checked = fixture.aa.check(source, true);
    REQUIRE(checked.has_value());
    CHECK(checked.value().allocations.size() == 1);
    CHECK(checked.value().blocks.size() == 2);
    // Said out loud in the notes, because a check that silently reported an
    // address which does not exist would be worse than no check.
    CHECK(std::any_of(checked.value().notes.begin(), checked.value().notes.end(),
                      [](const std::string& note) { return mentions(note, "this was a check"); }));

    CHECK(fixture.patches.patches().empty());
    CHECK(fixture.read(planted.address(), marker.size()) == marker);
}

TEST_CASE("assert refuses to run a script written against different bytes", "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);

    const std::string source =
        std::string("[ENABLE]\n") +
        "aobscanmodule(INJECT, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "alloc(newmem, 0x100, INJECT)\n"
        "assert(INJECT, 55 8B EC)\n"
        "newmem:\n"
        "  nop\n"
        "INJECT:\n"
        "  jmp newmem\n";

    const auto id = fixture.aa.add("guarded", source);
    auto enabled = fixture.aa.setEnabled(id, true);
    REQUIRE_FALSE(enabled.has_value());
    CHECK(mentions(enabled.error(), "assert failed"));
    CHECK(mentions(enabled.error(), "Nothing has been written"));

    CHECK(fixture.patches.patches().empty());
    CHECK(fixture.read(planted.address(), marker.size()) == marker);
    CHECK_FALSE(fixture.aa.find(id)->enabled);

    // The same assert, given the bytes that really are there, gets out of the
    // way entirely.
    const std::string honest =
        std::string("[ENABLE]\n") +
        "aobscanmodule(INJECT, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "assert(INJECT, DE AD BE EF)\n";
    CHECK(fixture.aa.check(honest, true).has_value());
}

TEST_CASE("a pattern that matches more than once is refused", "[aa][integration]") {
    Fixture fixture;

    // Four zero bytes occur all over a linked image. Taking the first would
    // produce a script that works today and patches something unrelated after
    // the next build of the target.
    const std::string source =
        std::string("[ENABLE]\n") + "aobscanmodule(X, " + helperModule + ", 00 00 00 00)\n";

    auto checked = fixture.aa.check(source, true);
    REQUIRE_FALSE(checked.has_value());
    CHECK(mentions(checked.error(), "matches"));
    CHECK(mentions(checked.error(), "Lengthen it"));
}

TEST_CASE("a pattern that is not there, and a module that is not loaded, are both named",
          "[aa][integration]") {
    Fixture fixture;

    const std::string missing =
        std::string("[ENABLE]\n") +
        "aobscanmodule(X, " + helperModule + ", DE AD BE EF CA FE BA BE 11 22 33 44)\n";
    auto notFound = fixture.aa.check(missing, true);
    REQUIRE_FALSE(notFound.has_value());
    CHECK(mentions(notFound.error(), "was not found"));

    const std::string absent = "[ENABLE]\naobscanmodule(X, nosuchmodule.dll, 90 90)\n";
    auto noModule = fixture.aa.check(absent, true);
    REQUIRE_FALSE(noModule.has_value());
    CHECK(mentions(noModule.error(), "not loaded"));
}

TEST_CASE("malformed scripts are refused with the line that is wrong", "[aa][integration]") {
    Fixture fixture;

    struct Case {
        const char* source;
        const char* fragment;
    };

    const Case cases[]{
        {"[ENABLE]\nalloc(newmem)\n", "alloc takes a name, a size"},
        {"[ENABLE]\nalloc(newmem, notanumber)\n", "is not a size"},
        {"[ENABLE]\nfrobnicate(1)\n", "is not a directive this assembler knows"},
        {"[ENABLE]\nlabel()\n", "label takes one name"},
        {"[ENABLE]\nalloc(newmem, 0x10)\nlabel(never)\nnewmem:\n  nop\n", "\"never\""},
        {"[ENABLE]\n  mov eax, 1\n", "no address to assemble at yet"},
        {"[ENABLE]\nalloc(newmem, 0x10)\nregistersymbol(ghost)\n", "the script never defines"},
        {"[ENABLE]\nalloc(newmem, 0x10)\nnewmem:\n  db zz\n", "not readable as data"},
        {"[ENABLE]\nalloc(newmem, 0x10)\nnewmem:\n  wibble eax, 1\n", "Line 4"},
        {"[ENABLE]\nalloc(newmem, 0x10)\nnewmem:\n  nop\nnotanaddress+zz:\n  nop\n",
         "is not somewhere this script can assemble at"},
    };

    for (const auto& one : cases) {
        INFO(one.source);
        auto checked = fixture.aa.check(one.source, true);
        REQUIRE_FALSE(checked.has_value());
        CHECK(mentions(checked.error(), one.fragment));
    }

    // Where the failure is a line rather than the script as a whole, the line is
    // named -- a script is edited in a box with no line numbers of its own.
    auto sizeless = fixture.aa.check("[ENABLE]\nalloc(newmem)\n", true);
    REQUIRE_FALSE(sizeless.has_value());
    CHECK(mentions(sizeless.error(), "Line 2"));
}

TEST_CASE("an address expression can be the place a section assembles at", "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);

    // What define(INJECT, <address>) leaves behind once it has substituted, and
    // a reasonable thing to write by hand. The label parser cannot take it --
    // it is not an identifier -- so it goes through the symbol table instead.
    const std::string source =
        "[ENABLE]\n"
        "define(INJECT, " + domain::toHex(planted.address()) + ")\n"
        "assert(INJECT, DE AD BE EF)\n"
        "INJECT:\n"
        "  nop\n"
        "  nop\n"
        "  nop\n";

    const auto id = fixture.aa.add("fixed address", source);
    REQUIRE(fixture.aa.setEnabled(id, true).has_value());
    CHECK(fixture.read(planted.address(), 3) == std::vector<std::uint8_t>{0x90, 0x90, 0x90});

    REQUIRE(fixture.aa.setEnabled(id, false).has_value());
    CHECK(fixture.read(planted.address(), marker.size()) == marker);
}

TEST_CASE("define substitutes its text before anything else reads the line", "[aa][integration]") {
    Fixture fixture;

    const std::string source =
        "[ENABLE]\n"
        "define(MAGIC, 0A0B0C0D)\n"
        "alloc(store, 0x20)\n"
        "registersymbol(store)\n"
        "store:\n"
        "  dd MAGIC\n"
        "[DISABLE]\n"
        "unregistersymbol(store)\n"
        "dealloc(store)\n";

    const auto id = fixture.aa.add("defined", source);
    REQUIRE(fixture.aa.setEnabled(id, true).has_value());

    const auto store = fixture.symbols.resolve(fixture.session(), "store");
    REQUIRE(store.has_value());
    CHECK(fixture.read(store.value(), 4) == std::vector<std::uint8_t>{0x0D, 0x0C, 0x0B, 0x0A});

    REQUIRE(fixture.aa.setEnabled(id, false).has_value());
}

TEST_CASE("a script cannot be edited while it is on", "[aa][integration]") {
    Fixture fixture;

    const std::string source =
        "[ENABLE]\n"
        "alloc(store, 0x20)\n"
        "store:\n"
        "  db 90\n"
        "[DISABLE]\n"
        "dealloc(store)\n";

    const auto id = fixture.aa.add("locked", source);
    REQUIRE(fixture.aa.setEnabled(id, true).has_value());

    // The [DISABLE] section that would put the target back is part of the text
    // being replaced, so allowing this is how a patch ends up with no way back.
    auto updated = fixture.aa.update(id, "locked", "[ENABLE]\n[DISABLE]\n");
    REQUIRE_FALSE(updated.has_value());
    CHECK(fixture.aa.find(id)->source == source);

    REQUIRE(fixture.aa.setEnabled(id, false).has_value());
    CHECK(fixture.aa.update(id, "locked", "[ENABLE]\n[DISABLE]\n").has_value());
    CHECK(fixture.aa.find(id)->source == "[ENABLE]\n[DISABLE]\n");
}

TEST_CASE("removing a script that is on puts the target back first", "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);

    const std::string source =
        std::string("[ENABLE]\n") +
        "aobscanmodule(INJECT, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "INJECT:\n"
        "  nop\n"
        "  nop\n";

    const auto id = fixture.aa.add("nops", source);
    REQUIRE(fixture.aa.setEnabled(id, true).has_value());
    CHECK(fixture.read(planted.address(), 2) == std::vector<std::uint8_t>{0x90, 0x90});

    REQUIRE(fixture.aa.remove(id).has_value());
    CHECK(fixture.aa.scripts().empty());
    CHECK(fixture.patches.patches().empty());
    CHECK(fixture.read(planted.address(), marker.size()) == marker);
}

TEST_CASE("a failure part-way through leaves nothing applied", "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);

    // Two blocks over the same bytes. The registry refuses the second -- its
    // "original" would be the first patch's output -- so the run fails after it
    // has already written something, which is exactly the case the rollback
    // exists for.
    const std::string source =
        std::string("[ENABLE]\n") +
        "aobscanmodule(A, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "aobscanmodule(B, " + helperModule + ", DE AD BE EF CA FE BA BE)\n"
        "A:\n"
        "  nop\n"
        "B:\n"
        "  nop\n";

    const auto id = fixture.aa.add("clashing", source);
    auto enabled = fixture.aa.setEnabled(id, true);
    REQUIRE_FALSE(enabled.has_value());

    CHECK_FALSE(fixture.aa.find(id)->enabled);
    CHECK(fixture.patches.patches().empty());
    CHECK(fixture.read(planted.address(), marker.size()) == marker);
}

TEST_CASE("disableAll switches off everything that is on", "[aa][integration]") {
    Fixture fixture;

    const std::string source =
        "[ENABLE]\n"
        "alloc(store, 0x20)\n"
        "store:\n"
        "  db 90\n"
        "[DISABLE]\n"
        "dealloc(store)\n";

    const auto first = fixture.aa.add("one", source);
    const auto second = fixture.aa.add("two", source);
    const auto never = fixture.aa.add("three", source);
    REQUIRE(fixture.aa.setEnabled(first, true).has_value());
    REQUIRE(fixture.aa.setEnabled(second, true).has_value());

    REQUIRE(fixture.aa.disableAll().has_value());
    CHECK_FALSE(fixture.aa.find(first)->enabled);
    CHECK_FALSE(fixture.aa.find(second)->enabled);
    CHECK_FALSE(fixture.aa.find(never)->enabled);
    // The list survives; only the effects are undone.
    CHECK(fixture.aa.scripts().size() == 3);

    fixture.aa.forgetAll();
    CHECK(fixture.aa.scripts().empty());
}

TEST_CASE("a script cannot run without a target", "[aa]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_asm::Assembler assembler;
    engine_patch::PatchRegistry patches(session);
    engine_symbols::SymbolTable symbols;
    engine_inject::Injector injector(session);
    engine_aa::AutoAssembler aa(session, assembler, patches, symbols, injector);

    auto checked = aa.check("[ENABLE]\nalloc(store, 0x20)\n", true);
    REQUIRE_FALSE(checked.has_value());
    CHECK(mentions(checked.error(), "Attach to a process"));

    // An empty section is not an error, though. A script whose [DISABLE] is
    // blank is a script that relies on the patch registry to undo it, which is
    // a perfectly ordinary thing to write.
    CHECK(aa.check("[ENABLE]\n[DISABLE]\n", false).has_value());
}

TEST_CASE("every template compiles as written", "[aa][integration]") {
    Fixture fixture;
    PlantedMarker planted(fixture);

    using Template = engine_aa::AutoAssembler::Template;
    for (const auto shape : {Template::AobInjection, Template::CodeCave, Template::FullInjection}) {
        const auto source =
            fixture.aa.makeTemplate(shape, planted.address(), marker, helperModule);
        INFO(source);
        // The templates are the first thing anyone runs, and a template that
        // does not compile teaches the reader that the tool is broken rather
        // than that their script is.
        CHECK(fixture.aa.check(source, true).has_value());
    }
}
