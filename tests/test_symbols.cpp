// Names for addresses.
//
// The expression parser is the part every address box in the tool depends on,
// so its refusals matter as much as its successes: a wrong answer here sends a
// reader to patch an address that is not the one they meant.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_symbols/SymbolTable.h"

using namespace ire;
using testsupport::AttachedHelper;

TEST_CASE("Plain addresses resolve with or without a target", "[symbols]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_symbols::SymbolTable table;

    auto resolved = table.resolve(session, "0x140001000");
    REQUIRE(resolved.has_value());
    CHECK(resolved.value() == 0x140001000);

    // Hexadecimal with or without the prefix, because "00400000" reading as
    // decimal would resolve silently to a completely different address.
    CHECK(table.resolve(session, "140001000").value() == 0x140001000);
    CHECK(table.resolve(session, "  0x1000  ").value() == 0x1000);
    CHECK_FALSE(table.resolve(session, "").has_value());
    CHECK_FALSE(table.resolve(session, "not an address").has_value());
}

TEST_CASE("Offsets are applied in order and are hexadecimal", "[symbols]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_symbols::SymbolTable table;

    CHECK(table.resolve(session, "0x1000+0x10").value() == 0x1010);
    CHECK(table.resolve(session, "0x1000+10").value() == 0x1010);
    CHECK(table.resolve(session, "0x1000-0x10").value() == 0xFF0);
    CHECK(table.resolve(session, "0x1000 + 0x10 - 0x8").value() == 0x1008);

    CHECK_FALSE(table.resolve(session, "0x1000+").has_value());
    CHECK_FALSE(table.resolve(session, "0x1000+zzz").has_value());
}

TEST_CASE("A module name resolves to its base, and module+offset to a point inside it",
          "[symbols][integration]") {
    AttachedHelper fixture;
    engine_symbols::SymbolTable table;

    auto base = table.resolve(fixture.session, "pointerlab_test_helper.exe");
    REQUIRE(base.has_value());
    CHECK(base.value() != 0);

    auto inside = table.resolve(fixture.session, "pointerlab_test_helper.exe+0x100");
    REQUIRE(inside.has_value());
    CHECK(inside.value() == base.value() + 0x100);

    // Case-insensitive, like the loader.
    CHECK(table.resolve(fixture.session, "POINTERLAB_TEST_HELPER.EXE").value() == base.value());

    auto missing = table.resolve(fixture.session, "definitely_not_loaded.dll+0x10");
    REQUIRE_FALSE(missing.has_value());
    CHECK_FALSE(missing.error().empty());
}

TEST_CASE("An export resolves out of the target's own module", "[symbols][integration]") {
    AttachedHelper fixture;
    engine_symbols::SymbolTable table;

    // Split at the *last* separator, so the module keeps its extension rather
    // than the name splitting into "kernel32" and "dll.LoadLibraryW".
    auto dotted = table.resolve(fixture.session, "kernel32.dll.LoadLibraryW");
    REQUIRE(dotted.has_value());
    CHECK(dotted.value() != 0);

    // Debugger notation, and the extension left off, both mean the same thing.
    CHECK(table.resolve(fixture.session, "kernel32!LoadLibraryW").value() == dotted.value());
    CHECK(table.resolve(fixture.session, "kernel32.LoadLibraryW").value() == dotted.value());
    CHECK(table.resolve(fixture.session, "kernel32.LoadLibraryW+0x10").value() == dotted.value() + 0x10);

    auto absent = table.resolve(fixture.session, "kernel32.NoSuchExportExists");
    REQUIRE_FALSE(absent.has_value());
    // Naming the module as loaded but lacking the export sends the reader to
    // the right place; "not a valid address" would not.
    CHECK(absent.error().find("NoSuchExportExists") != std::string::npos);
}

TEST_CASE("A defined symbol can be used anywhere an address can", "[symbols][integration]") {
    AttachedHelper fixture;
    engine_symbols::SymbolTable table;

    auto defined = table.define(fixture.session, "health", "pointerlab_test_helper.exe+0x100");
    REQUIRE(defined.has_value());

    CHECK(table.resolve(fixture.session, "health").value() == defined.value());
    CHECK(table.resolve(fixture.session, "health+0x8").value() == defined.value() + 8);
    // Names are matched case-insensitively, so a symbol typed back in a
    // different case is still the same symbol.
    CHECK(table.resolve(fixture.session, "HEALTH").value() == defined.value());

    REQUIRE(table.find("health").has_value());
    CHECK(table.find("health")->expression == "pointerlab_test_helper.exe+0x100");

    // Redefining replaces rather than duplicating.
    REQUIRE(table.define(fixture.session, "health", "pointerlab_test_helper.exe+0x200").has_value());
    CHECK(table.symbols().size() == 1);
    CHECK(table.resolve(fixture.session, "health").value() == defined.value() + 0x100);

    CHECK(table.undefine("HEALTH"));
    CHECK_FALSE(table.undefine("health"));
    CHECK(table.symbols().empty());
}

TEST_CASE("A symbol name that could be read as something else is refused", "[symbols]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_symbols::SymbolTable table;

    // "beef" is a perfectly good hexadecimal address, and a symbol by that name
    // would shadow it everywhere.
    CHECK_FALSE(table.define(session, "beef", "0x1000").has_value());
    // A name containing an operator could never be typed back in: the parser
    // would split it into terms.
    CHECK_FALSE(table.define(session, "my+name", "0x1000").has_value());
    CHECK_FALSE(table.define(session, "  ", "0x1000").has_value());
    // A definition that does not resolve is not recorded at all, so a symbol in
    // the list is always one that meant something at least once.
    CHECK_FALSE(table.define(session, "ghost", "not_a_module.dll+0x10").has_value());
    CHECK(table.symbols().empty());
}

TEST_CASE("describe names an address inside a module and says nothing about one that is not",
          "[symbols][integration]") {
    AttachedHelper fixture;
    engine_symbols::SymbolTable table;

    const auto base = table.resolve(fixture.session, "pointerlab_test_helper.exe").value();

    CHECK(table.describe(fixture.session, base).find("pointerlab_test_helper.exe") != std::string::npos);
    const auto inside = table.describe(fixture.session, base + 0x100);
    CHECK(inside.find("pointerlab_test_helper.exe") != std::string::npos);
    CHECK(inside.find("0x100") != std::string::npos);

    CHECK(engine_symbols::SymbolTable::isStatic(fixture.session, base));
    CHECK(engine_symbols::SymbolTable::moduleAt(fixture.session, base).has_value());

    // The helper's watched value is a global, but its scratch area is reached
    // through the same image; an address far outside every module is the case
    // that must produce nothing rather than a made-up name.
    CHECK(table.describe(fixture.session, 0x10).empty());
    CHECK_FALSE(engine_symbols::SymbolTable::isStatic(fixture.session, 0x10));

    // A user symbol is more specific than a module offset, so it wins.
    REQUIRE(table.define("entrypoint", base).has_value());
    CHECK(table.describe(fixture.session, base) == "entrypoint");
}

TEST_CASE("Nothing that names the target resolves without one", "[symbols]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_symbols::SymbolTable table;

    auto resolved = table.resolve(session, "client.dll+0x10");
    REQUIRE_FALSE(resolved.has_value());
    // "Nothing is attached" is the actionable answer; "not a valid address"
    // would send the reader to check their typing instead.
    CHECK(resolved.error().find("attached") != std::string::npos);
}
