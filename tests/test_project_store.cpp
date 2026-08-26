#include <catch2/catch_test_macros.hpp>

#include "storage/ProjectStore.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace ire;

namespace {

// Each test writes into its own file under the temp directory and removes it
// again, so the suite never depends on leftover state.
class TempFile {
public:
    explicit TempFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("pointerlab_test_" + name)) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write(const std::string& contents) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << contents;
    }

private:
    std::filesystem::path path_;
};

domain::AddressEntry makeEntry(std::uint64_t id, std::uintptr_t address, std::string description) {
    domain::AddressEntry entry;
    entry.id = id;
    entry.address = address;
    entry.type = domain::ValueType::Int32;
    entry.description = std::move(description);
    entry.group = "Default";
    entry.hotkey = "F1";
    entry.frozen = true;
    entry.frozenValue = {0x01, 0x02, 0x03, 0x04};
    return entry;
}

} // namespace

TEST_CASE("A saved project reloads with identical entries", "[storage]") {
    const TempFile file("roundtrip.iretable");
    const storage::ProjectStore store;

    storage::ProjectTable table;
    table.lastPid = 4321;
    table.lastProcessName = L"target.exe";
    table.entries.push_back(makeEntry(1, 0x140001000ull, "player health"));
    table.entries.push_back(makeEntry(2, 0x7FF6DEADBEEFull, "ammo"));

    REQUIRE(store.save(file.path(), table).has_value());

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    const auto& result = loaded.value();

    CHECK(result.lastPid == 4321);
    CHECK(result.lastProcessName == L"target.exe");
    REQUIRE(result.entries.size() == 2);

    for (std::size_t i = 0; i < table.entries.size(); ++i) {
        CHECK(result.entries[i].id == table.entries[i].id);
        CHECK(result.entries[i].address == table.entries[i].address);
        CHECK(result.entries[i].type == table.entries[i].type);
        CHECK(result.entries[i].description == table.entries[i].description);
        CHECK(result.entries[i].group == table.entries[i].group);
        CHECK(result.entries[i].hotkey == table.entries[i].hotkey);
        CHECK(result.entries[i].frozen == table.entries[i].frozen);
        CHECK(result.entries[i].frozenValue == table.entries[i].frozenValue);
    }
}

TEST_CASE("Delimiters and newlines in text survive a round trip", "[storage]") {
    const TempFile file("escaping.iretable");
    const storage::ProjectStore store;

    // '|' is the field separator and a raw newline would split the record.
    const std::string awkward = "pipe | backslash \\ newline \n carriage \r end";

    storage::ProjectTable table;
    table.entries.push_back(makeEntry(7, 0x1000, awkward));
    table.entries.back().group = "a|b";

    REQUIRE(store.save(file.path(), table).has_value());
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].description == awkward);
    CHECK(loaded.value().entries[0].group == "a|b");
}

TEST_CASE("An empty table round trips", "[storage]") {
    const TempFile file("empty.iretable");
    const storage::ProjectStore store;

    const storage::ProjectTable table;
    REQUIRE(store.save(file.path(), table).has_value());
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().entries.empty());
}

TEST_CASE("Version 1 files are still readable", "[storage]") {
    const TempFile file("v1.iretable");
    file.write(
        "IRETABLE 1\n"
        "pid|1234\n"
        "process|old.exe\n"
        "entry|1|140001000|i32|1|health|Default|F2|0A0B0C0D\n");

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().lastPid == 1234);
    CHECK(loaded.value().entries[0].address == 0x140001000ull);
    CHECK(loaded.value().entries[0].type == domain::ValueType::Int32);
    CHECK(loaded.value().entries[0].description == "health");
    CHECK(loaded.value().entries[0].hotkey == "F2");
}

// Persistence depends on this pair being symmetric: if a type's written name
// does not parse back, the entry silently takes on the wrong width.
TEST_CASE("Every value type survives a save and load", "[storage]") {
    const TempFile file("types.iretable");
    const storage::ProjectStore store;

    storage::ProjectTable table;
    std::uint64_t id = 1;
    for (const auto type : domain::valueTypes()) {
        auto entry = makeEntry(id, 0x100000ull + id * 0x10, "entry");
        entry.type = type;
        entry.frozenValue.clear(); // width varies by type
        table.entries.push_back(std::move(entry));
        ++id;
    }

    REQUIRE(store.save(file.path(), table).has_value());
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == table.entries.size());
    for (std::size_t i = 0; i < table.entries.size(); ++i) {
        CHECK(loaded.value().entries[i].type == table.entries[i].type);
    }
}

TEST_CASE("An entry with an unrecognised type is skipped, not silently retyped", "[storage]") {
    const TempFile file("badtype.iretable");
    file.write(
        "IRETABLE 2\n"
        "entry|1|140001000|Float|1|wrong type name|Default|F1|0A0B0C0D\n"
        "entry|2|140002000|f32|0|correct|Default|F2|0000803F\n");

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].id == 2);
    CHECK(loaded.value().entries[0].type == domain::ValueType::Float);
}

// Editors and PowerShell add a UTF-8 BOM without asking; it must not make a
// perfectly good project file look corrupt.
TEST_CASE("A leading UTF-8 BOM is tolerated", "[storage]") {
    const TempFile file("bom.iretable");
    file.write(
        "\xEF\xBB\xBF"
        "IRETABLE 2\n"
        "pid|99\n"
        "entry|1|140001000|i32|1|health|Default|F1|0A0B0C0D\n");

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().lastPid == 99);
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].address == 0x140001000ull);
}

TEST_CASE("A missing file reports an error instead of throwing", "[storage]") {
    const storage::ProjectStore store;
    const auto missing = std::filesystem::temp_directory_path() / "pointerlab_does_not_exist.iretable";
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    auto loaded = store.load(missing);
    CHECK_FALSE(loaded.has_value());
    CHECK_FALSE(loaded.error().empty());
}

TEST_CASE("A file with the wrong header is rejected", "[storage]") {
    const TempFile file("wrongheader.iretable");
    file.write("NOT A PROJECT\nentry|1|1000|Int32|0|x|y|z|00\n");

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    CHECK_FALSE(loaded.has_value());
}

// Regression: these numeric fields were parsed with bare std::stoull, so a
// hand-edited or truncated file threw out of load(). An uncaught C++ exception
// also bypasses the SEH crash filter, so it produced no crash log either.
TEST_CASE("Malformed numeric fields are skipped rather than throwing", "[storage]") {
    const TempFile file("malformed.iretable");
    file.write(
        "IRETABLE 2\n"
        "pid|not-a-number\n"
        "entry|abc|140001000|i32|1|bad id|g|F1|00\n"
        "entry|2|zzzzzz|i32|1|bad address|g|F1|00\n"
        "entry|3|99999999999999999999999999|i32|1|overflow|g|F1|00\n"
        "entry|4|140002000|i32|1|good|g|F3|0A0B0C0D\n"
        "garbage line with no fields\n");

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());

    // Only the well-formed row survives; the rest are skipped.
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].id == 4);
    CHECK(loaded.value().entries[0].address == 0x140002000ull);
    CHECK(loaded.value().entries[0].description == "good");
}

TEST_CASE("Truncated entry rows are ignored", "[storage]") {
    const TempFile file("truncated.iretable");
    file.write(
        "IRETABLE 2\n"
        "entry|1|1000\n"
        "entry|\n"
        "entry\n");

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().entries.empty());
}

// Version 3 added pointer chains. An entry that came from a pointer scan is
// worthless if only its resolved address is saved: that address belongs to a
// process that has since exited.
TEST_CASE("A pointer chain survives a save and load", "[storage]") {
    const TempFile file("chain.iretable");
    const storage::ProjectStore store;

    domain::PointerChain chain;
    chain.moduleName = L"game.exe";
    chain.moduleOffset = 0x1A2B3C;
    chain.offsets = {0x10, 0x0, 0x48};
    chain.moduleBase = 0x140000000; // display only, deliberately not persisted

    storage::ProjectTable table;
    auto entry = makeEntry(1, 0x7FF700001234ull, "health");
    entry.chain = chain;
    table.entries.push_back(std::move(entry));

    REQUIRE(store.save(file.path(), table).has_value());
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);

    const auto& restored = loaded.value().entries[0];
    REQUIRE(restored.chain.has_value());
    CHECK(restored.chain->moduleName == L"game.exe");
    CHECK(restored.chain->moduleOffset == 0x1A2B3Cull);
    CHECK(restored.chain->offsets == std::vector<std::ptrdiff_t>{0x10, 0x0, 0x48});
    // The saved address came from a process that no longer exists, so the entry
    // must start out unresolved rather than claiming a stale address is live.
    CHECK_FALSE(restored.resolved);
}

TEST_CASE("An entry without a chain stays a plain fixed address", "[storage]") {
    const TempFile file("nochain.iretable");
    const storage::ProjectStore store;

    storage::ProjectTable table;
    table.entries.push_back(makeEntry(1, 0x140001000, "static"));

    REQUIRE(store.save(file.path(), table).has_value());
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK_FALSE(loaded.value().entries[0].chain.has_value());
    CHECK(loaded.value().entries[0].resolved);
    CHECK(loaded.value().entries[0].address == 0x140001000ull);
}

// Older files have no chain fields at all, and must keep loading as they did.
TEST_CASE("Version 1 and 2 files still load without chains", "[storage]") {
    const TempFile file("legacy.iretable");
    const storage::ProjectStore store;

    file.write(
        "IRETABLE 2\n"
        "entry|7|140004000|i32|0|old entry|g|F5|01020304\n");
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].address == 0x140004000ull);
    CHECK_FALSE(loaded.value().entries[0].chain.has_value());
    CHECK(loaded.value().entries[0].resolved);
}

TEST_CASE("A malformed chain degrades to a fixed address rather than failing", "[storage]") {
    const TempFile file("badchain.iretable");
    const storage::ProjectStore store;

    file.write(
        "IRETABLE 3\n"
        "entry|1|140001000|i32|0|bad offsets|g||00|game.exe|1000|nothex\n"
        "entry|2|140002000|i32|0|bad module offset|g||00|game.exe|zzzz|10\n"
        "entry|3|140003000|i32|0|no offsets|g||00|game.exe|1000|\n"
        "entry|4|140004000|i32|0|good|g||00|game.exe|1000|10,20\n");

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    // The rows are all kept: a broken chain costs the chain, not the entry.
    REQUIRE(loaded.value().entries.size() == 4);
    CHECK_FALSE(loaded.value().entries[0].chain.has_value());
    CHECK_FALSE(loaded.value().entries[1].chain.has_value());
    // Not broken: "module+offset, no dereferencing" is how a static address is
    // written down, and manual entry produces exactly that.
    REQUIRE(loaded.value().entries[2].chain.has_value());
    CHECK(loaded.value().entries[2].chain->offsets.empty());
    CHECK(loaded.value().entries[2].chain->moduleRooted());
    REQUIRE(loaded.value().entries[3].chain.has_value());
    CHECK(loaded.value().entries[3].chain->offsets == std::vector<std::ptrdiff_t>{0x10, 0x20});
}

TEST_CASE("Symbols survive a round trip as expressions, not addresses", "[storage]") {
    const TempFile file("symbols.iretable");
    const storage::ProjectStore store;

    storage::ProjectTable table;
    table.symbols.push_back({"health", "game.exe+0x4A2C10"});
    table.symbols.push_back({"loadlib", "kernel32.LoadLibraryW"});
    REQUIRE(store.save(file.path(), table).has_value());

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().symbols.size() == 2);
    CHECK(loaded.value().symbols[0].name == "health");
    // The expression, not the address it resolved to. An address from a
    // previous run names nothing once ASLR has moved the module.
    CHECK(loaded.value().symbols[0].expression == "game.exe+0x4A2C10");
    CHECK(loaded.value().symbols[1].expression == "kernel32.LoadLibraryW");
}

TEST_CASE("Scripts survive a round trip with their line breaks intact", "[storage]") {
    const TempFile file("scripts.iretable");
    const storage::ProjectStore store;

    // The whole source on one line of the file, with newlines and the field
    // separator escaped. A script that came back with its lines run together
    // would still look plausible in the editor and assemble to nonsense.
    const std::string source =
        "[ENABLE]\n"
        "aobscanmodule(INJECT, game.exe, 89 46 04 8B ?? 08)\n"
        "alloc(newmem, 0x100, INJECT)\n"
        "newmem:\n"
        "  mov [esi+04], eax  // a | in a comment must survive too\n"
        "[DISABLE]\n"
        "dealloc(newmem)\n";

    storage::ProjectTable table;
    table.scripts.push_back({"infinite health", source});
    table.scripts.push_back({"empty", ""});
    REQUIRE(store.save(file.path(), table).has_value());

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().scripts.size() == 2);
    CHECK(loaded.value().scripts[0].name == "infinite health");
    CHECK(loaded.value().scripts[0].source == source);
    // A named script the user has not written yet is still theirs to keep.
    CHECK(loaded.value().scripts[1].name == "empty");
    CHECK(loaded.value().scripts[1].source.empty());
}

TEST_CASE("Structure layouts survive a round trip, negative offsets included", "[storage]") {
    const TempFile file("structures.iretable");
    const storage::ProjectStore store;

    storage::ProjectTable table;
    domain::Structure player;
    player.name = "Player";
    // Above the start of the object: the start is always a guess, and one that
    // turned out to be four bytes late must still round-trip.
    player.fields.push_back({-4, domain::ValueType::Int32, 0, "header"});
    player.fields.push_back({0x00, domain::ValueType::UInt64, 0, "vtable"});
    player.fields.push_back({0x08, domain::ValueType::Float, 0, "health"});
    player.fields.push_back({0x10, domain::ValueType::Bytes, 12, "name"});
    table.structures.push_back(player);

    domain::Structure weapon;
    weapon.name = "Weapon";
    weapon.fields.push_back({0x00, domain::ValueType::Int32, 0, "ammo"});
    table.structures.push_back(weapon);

    REQUIRE(store.save(file.path(), table).has_value());

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().structures.size() == 2);

    const auto& first = loaded.value().structures[0];
    CHECK(first.name == "Player");
    REQUIRE(first.fields.size() == 4);
    CHECK(first.fields[0].offset == -4);
    CHECK(first.fields[0].name == "header");
    CHECK(first.fields[1].type == domain::ValueType::UInt64);
    // The length only means anything for a variable-width type, and it is the
    // only place it can come from.
    CHECK(first.fields[3].type == domain::ValueType::Bytes);
    CHECK(first.fields[3].length == 12);
    CHECK(first.fields[3].size() == 12);

    // Fields belong to the structure above them, so a second one does not
    // inherit the first one's.
    CHECK(loaded.value().structures[1].name == "Weapon");
    CHECK(loaded.value().structures[1].fields.size() == 1);
}

TEST_CASE("A field record before any structure is ignored rather than misfiled", "[storage]") {
    const TempFile file("orphanfield.iretable");
    {
        std::ofstream out(file.path(), std::ios::binary | std::ios::trunc);
        out << "IRETABLE 3\n";
        // A layout attached to the wrong structure is worse than a missing one,
        // so this is dropped rather than given to whatever comes next.
        out << "field|0|i32|0|orphan\n";
        out << "struct|Player\n";
        out << "field|4|f32|0|health\n";
    }

    const storage::ProjectStore store;
    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().structures.size() == 1);
    REQUIRE(loaded.value().structures[0].fields.size() == 1);
    CHECK(loaded.value().structures[0].fields[0].name == "health");
}

TEST_CASE("A chain with an absolute base round-trips and stays absolute", "[storage]") {
    const TempFile file("absolutechain.iretable");
    const storage::ProjectStore store;

    storage::ProjectTable table;
    auto entry = makeEntry(1, 0x140001000, "manual chain");
    domain::PointerChain chain;
    // No module name: a base the user typed by hand, which resolves but does
    // not survive a restart.
    chain.moduleOffset = 0x7FF000001000ull;
    chain.offsets = {0x10, -0x8};
    entry.chain = chain;
    table.entries.push_back(entry);
    REQUIRE(store.save(file.path(), table).has_value());

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    const auto& reloaded = loaded.value().entries[0].chain;
    REQUIRE(reloaded.has_value());
    CHECK_FALSE(reloaded->moduleRooted());
    CHECK(reloaded->moduleOffset == 0x7FF000001000ull);
    // Including the negative one, which steps backwards through a structure.
    CHECK(reloaded->offsets == std::vector<std::ptrdiff_t>{0x10, -0x8});
}

TEST_CASE("An unrecognised record is skipped rather than failing the load", "[storage]") {
    const TempFile file("unknownrecord.iretable");
    const storage::ProjectStore store;

    // The property that lets new records be added without a version bump: an
    // older build reading a newer file loses the line, not the table.
    file.write(
        "IRETABLE 3\n"
        "somethingnew|whatever|else\n"
        "entry|1|140001000|i32|0|kept|g||00\n");

    auto loaded = store.load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].description == "kept");
}
