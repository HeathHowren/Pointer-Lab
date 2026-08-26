// Tests for the trainer generator.
//
// Generation is pure: options in, three files of text out. So these tests read
// the generated text rather than building it, and check the things that would
// make a generated trainer wrong rather than merely ugly -- that a module-rooted
// chain is emitted as module+offset and an absolute address is emitted as one
// and labelled, that the frozen bytes survive intact, that a hotkey becomes the
// right virtual key, and that nothing is silently dropped.

#include <catch2/catch_test_macros.hpp>

#include "engine_export/TrainerExport.h"
#include "infra/Paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ire;
using engine_export::TrainerExport;
using engine_export::TrainerOptions;

namespace {

std::string fileNamed(const std::vector<engine_export::GeneratedFile>& files, const std::string& name) {
    for (const auto& file : files) {
        if (file.name == name) {
            return file.contents;
        }
    }
    return {};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

domain::AddressEntry rootedEntry() {
    domain::AddressEntry entry;
    entry.address = 0x140001234;
    entry.type = domain::ValueType::Int32;
    entry.frozenValue = {0x99, 0x00, 0x00, 0x00};
    entry.description = "Health";
    entry.hotkey = "F3";
    domain::PointerChain chain;
    chain.moduleName = L"game.exe";
    chain.moduleOffset = 0x1F2C40;
    chain.offsets = {0x10, 0x0, 0x2C};
    chain.moduleBase = 0x140000000;
    entry.chain = chain;
    return entry;
}

domain::AddressEntry absoluteEntry() {
    domain::AddressEntry entry;
    entry.address = 0x1DEADBEEF;
    entry.type = domain::ValueType::Float;
    entry.frozenValue = {0x00, 0x00, 0x80, 0x3F}; // 1.0f
    entry.description = "Ammo";
    return entry;
}

TrainerOptions optionsWith(std::vector<domain::AddressEntry> entries) {
    TrainerOptions options;
    options.name = "My Trainer";
    options.processName = L"game.exe";
    options.bitness = domain::Bitness::X64;
    options.entries = std::move(entries);
    return options;
}

} // namespace

TEST_CASE("A name becomes something a compiler and a file system both accept", "[export]") {
    CHECK(TrainerExport::identifier("My Trainer") == "My_Trainer");
    CHECK(TrainerExport::identifier("assault-cube") == "assault_cube");
    CHECK(TrainerExport::identifier("Half-Life 2: Episode One") == "Half_Life_2_Episode_One");
    // Trailing punctuation would otherwise leave a trailing underscore.
    CHECK(TrainerExport::identifier("My Trainer ") == "My_Trainer");
    CHECK(TrainerExport::identifier("!!!hi!!!") == "hi");
    // A C++ identifier cannot begin with a digit, and CMake project names of
    // pure punctuation are not names at all.
    CHECK(TrainerExport::identifier("2fast") == "_2fast");
    CHECK(TrainerExport::identifier("***") == "Trainer");
    CHECK(TrainerExport::identifier("") == "Trainer");
}

TEST_CASE("The generator emits three files, and they are the project", "[export]") {
    const auto files = TrainerExport::generate(optionsWith({rootedEntry()}));
    REQUIRE(files.size() == 3);

    const auto source = fileNamed(files, "main.cpp");
    const auto cmake = fileNamed(files, "CMakeLists.txt");
    const auto readme = fileNamed(files, "README.md");
    REQUIRE_FALSE(source.empty());
    REQUIRE_FALSE(cmake.empty());
    REQUIRE_FALSE(readme.empty());

    CHECK(contains(source, "int main()"));
    CHECK(contains(source, "findProcess"));
    CHECK(contains(source, "moduleBase"));
    CHECK(contains(source, "ReadProcessMemory"));
    CHECK(contains(source, "WriteProcessMemory"));
    // The sanitised name, not the raw one -- "My Trainer" is not a target name.
    CHECK(contains(cmake, "project(My_Trainer CXX)"));
    CHECK(contains(cmake, "add_executable(My_Trainer main.cpp)"));
    // The architecture is load-bearing for pointer width, so it has to be in
    // the build instructions rather than left to whatever the default is.
    CHECK(contains(cmake, "-A x64"));
    CHECK(contains(readme, "cmake -S . -B build -A x64"));
}

TEST_CASE("A 32-bit target generates a 32-bit build", "[export]") {
    auto options = optionsWith({rootedEntry()});
    options.bitness = domain::Bitness::X86;
    const auto files = TrainerExport::generate(options);

    CHECK(contains(fileNamed(files, "CMakeLists.txt"), "-A Win32"));
    CHECK(contains(fileNamed(files, "README.md"), "cmake -S . -B build -A Win32"));
    CHECK_FALSE(contains(fileNamed(files, "CMakeLists.txt"), "-A x64"));
}

TEST_CASE("A module-rooted chain is emitted as module and offset", "[export]") {
    const auto source = fileNamed(TrainerExport::generate(optionsWith({rootedEntry()})), "main.cpp");

    // The whole reason the export is worth anything: an entry recorded as
    // module+offset resolves again after the game restarts, and an absolute
    // address does not.
    CHECK(contains(source, "L\"game.exe\", 0x1F2C40"));
    CHECK(contains(source, "const ptrdiff_t cheat0_offsets[] = { 0x10, 0x0, 0x2C };"));
    CHECK(contains(source, "cheat0_offsets, 3"));
    // The literal base at scan time must not appear as something the trainer
    // uses -- baking 0x140000000 in would defeat the chain entirely.
    CHECK_FALSE(contains(source, "0x140001234"));
}

TEST_CASE("An absolute address is emitted as one and labelled as a hazard", "[export]") {
    const auto files = TrainerExport::generate(optionsWith({absoluteEntry()}));
    const auto source = fileNamed(files, "main.cpp");
    const auto readme = fileNamed(files, "README.md");

    CHECK(contains(source, "nullptr, 0x1DEADBEEF"));
    CHECK(contains(source, "nullptr, 0"));
    // Not a silent difference. A generated trainer full of absolute addresses
    // works exactly once and then writes to whatever ASLR put there instead.
    CHECK(contains(source, "absolute address: valid only while this run of the game lasts"));
    CHECK(contains(readme, "A warning about the absolute addresses"));
    CHECK(contains(readme, "(absolute)"));
}

TEST_CASE("A module-rooted-only table carries no absolute-address warning", "[export]") {
    const auto readme = fileNamed(TrainerExport::generate(optionsWith({rootedEntry()})), "README.md");
    CHECK_FALSE(contains(readme, "A warning about the absolute addresses"));
}

TEST_CASE("The frozen bytes are what the trainer writes", "[export]") {
    domain::AddressEntry entry = rootedEntry();
    entry.type = domain::ValueType::Bytes;
    entry.frozenValue = {0x90, 0x0F, 0xFF, 0x00, 0x7B};
    const auto source = fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp");

    // Two digits each and uppercase, because a byte array read back by a human
    // against a hex editor is the point of generating source.
    CHECK(contains(source, "const unsigned char cheat0_value[] = { 0x90, 0x0F, 0xFF, 0x00, 0x7B };"));
    CHECK(contains(source, "cheat0_value, sizeof(cheat0_value)"));
}

TEST_CASE("A function-key hotkey becomes its virtual key code", "[export]") {
    auto entry = rootedEntry();

    entry.hotkey = "F1";
    CHECK(contains(fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp"), "\", 0x70, "));
    entry.hotkey = "F12";
    CHECK(contains(fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp"), "\", 0x7B, "));

    // Anything the address list cannot express becomes always-on rather than a
    // key the trainer would wait forever for.
    entry.hotkey = "";
    const auto none = fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp");
    CHECK(contains(none, "\", 0, "));
    entry.hotkey = "F13";
    CHECK(contains(fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp"), "\", 0, "));
    entry.hotkey = "Ctrl+H";
    CHECK(contains(fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp"), "\", 0, "));
}

TEST_CASE("An entry with no frozen value is listed rather than dropped", "[export]") {
    domain::AddressEntry watched;
    watched.address = 0x140005000;
    watched.description = "Score";
    // No frozenValue: something to watch, not something to write.

    const auto files = TrainerExport::generate(optionsWith({rootedEntry(), watched}));
    const auto source = fileNamed(files, "main.cpp");
    const auto readme = fileNamed(files, "README.md");

    // One cheat, not two -- there is nothing to write for the second.
    CHECK(contains(source, "cheat0_value"));
    CHECK_FALSE(contains(source, "cheat1_value"));
    // But the person is told, because an entry that vanished from the export
    // with no explanation reads as a bug in the generator.
    CHECK(contains(readme, "## Not exported"));
    CHECK(contains(readme, "- Score"));
    CHECK(contains(readme, "1 entry had no recorded value"));
}

TEST_CASE("A table with nothing frozen still generates a project that builds", "[export]") {
    domain::AddressEntry watched;
    watched.address = 0x140005000;
    const auto files = TrainerExport::generate(optionsWith({watched}));
    const auto source = fileNamed(files, "main.cpp");

    // An empty g_cheats[] would not compile, and the range-for over it in the
    // main loop would not either. The generator emits the explanation instead.
    CHECK(contains(source, "int main()"));
    CHECK(contains(source, "nothing to write"));
    CHECK_FALSE(contains(source, "Cheat g_cheats[] = {\n    {"));
    CHECK(contains(fileNamed(files, "README.md"), "Nothing yet."));
}

TEST_CASE("A description with quotes cannot break out of the generated string", "[export]") {
    auto entry = rootedEntry();
    entry.description = "He said \"hi\" \\ then\nleft";
    const auto source = fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp");

    CHECK(contains(source, "He said \\\"hi\\\" \\\\ then left"));
    // A raw newline inside a narrow string literal does not compile, and a raw
    // quote silently changes what the rest of the line means.
    const auto start = source.find("Cheat g_cheats[]");
    REQUIRE(start != std::string::npos);
    const auto table = source.substr(start, source.find("};", start) - start);
    CHECK_FALSE(contains(table, "said \"hi\""));
}

TEST_CASE("An entry with no description is named by its address", "[export]") {
    auto entry = rootedEntry();
    entry.description.clear();
    const auto source = fileNamed(TrainerExport::generate(optionsWith({entry})), "main.cpp");
    CHECK(contains(source, domain::toHex(entry.address)));
}

TEST_CASE("Writing produces the files, and refuses to write over them", "[export]") {
    const auto directory = infra::Paths::appData() / "test-trainer-export";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);

    const auto options = optionsWith({rootedEntry()});
    auto written = TrainerExport::write(directory, options);
    REQUIRE(written.has_value());
    CHECK(written.value() == 3);
    CHECK(std::filesystem::exists(directory / "main.cpp"));
    CHECK(std::filesystem::exists(directory / "CMakeLists.txt"));
    CHECK(std::filesystem::exists(directory / "README.md"));

    // The likeliest reason for a collision is a trainer the user has since
    // edited, so overwriting it silently is the one unrecoverable mistake this
    // feature could make.
    auto again = TrainerExport::write(directory, options);
    REQUIRE_FALSE(again.has_value());
    CHECK(contains(again.error(), "already exists"));

    // Refused before anything was written, not partway through: the file that
    // was there is untouched.
    std::ifstream check(directory / "main.cpp", std::ios::binary);
    std::ostringstream contents;
    contents << check.rdbuf();
    CHECK(contains(contents.str(), "int main()"));

    std::filesystem::remove_all(directory, ec);
}
