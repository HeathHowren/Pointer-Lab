// Preferences that persist between runs.
//
// The failure that matters here is not losing a setting, it is a settings file
// that stops the application behaving sensibly. So every malformed input below
// has to land on the documented defaults rather than on something arbitrary, and
// a file from a newer version has to stay readable.

#include <catch2/catch_test_macros.hpp>

#include "infra/Settings.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace ire;

namespace {

// A settings file in its own temporary directory, removed afterwards.
class TempSettings {
public:
    TempSettings() {
        path_ = std::filesystem::temp_directory_path() /
                ("pointerlab_settings_test_" + std::to_string(counter_++) + ".ini");
        remove();
    }
    ~TempSettings() { remove(); }

    TempSettings(const TempSettings&) = delete;
    TempSettings& operator=(const TempSettings&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write(const std::string& contents) const {
        std::ofstream out(path_, std::ios::trunc | std::ios::binary);
        out << contents;
    }

    [[nodiscard]] std::string read() const {
        std::ifstream in(path_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

private:
    void remove() const {
        std::error_code code;
        std::filesystem::remove(path_, code);
    }

    std::filesystem::path path_;
    static inline int counter_ = 0;
};

} // namespace

TEST_CASE("Settings round-trip through a file unchanged", "[settings]") {
    TempSettings file;

    infra::Settings written;
    written.scanMaxResults = 250000;
    written.scanFloatEpsilon = 0.125;
    written.scanWritableOnly = true;
    written.scanExecutableOnly = false;
    written.scanTypeIndex = 8;
    written.pointerDepth = 6;
    written.pointerTypeIndex = 2;
    written.showMemoryViewer = false;
    written.showLogs = false;
    written.showLuaConsole = false;

    REQUIRE(infra::saveSettings(file.path(), written));
    const auto read = infra::loadSettings(file.path());

    CHECK(read.scanMaxResults == 250000);
    CHECK(read.scanFloatEpsilon == 0.125);
    CHECK(read.scanWritableOnly);
    CHECK_FALSE(read.scanExecutableOnly);
    CHECK(read.scanTypeIndex == 8);
    CHECK(read.pointerDepth == 6);
    CHECK(read.pointerTypeIndex == 2);
    CHECK_FALSE(read.showMemoryViewer);
    CHECK_FALSE(read.showLogs);
    CHECK_FALSE(read.showLuaConsole);
    // Untouched fields keep their defaults rather than being zeroed.
    CHECK(read.showBreakpoints);
    CHECK(read.showModules);
}

// A float tolerance that drifted a little on every save would be its own quiet
// bug, so the written text has to convert back to the identical double.
TEST_CASE("A float tolerance survives the round trip exactly", "[settings]") {
    TempSettings file;

    infra::Settings written;
    written.scanFloatEpsilon = 0.1 + 0.2; // Deliberately not representable.
    REQUIRE(infra::saveSettings(file.path(), written));

    CHECK(infra::loadSettings(file.path()).scanFloatEpsilon == written.scanFloatEpsilon);
}

TEST_CASE("A settings file that cannot be trusted falls back to defaults", "[settings]") {
    TempSettings file;
    const infra::Settings defaults;

    SECTION("no file at all") {
        const auto read = infra::loadSettings(file.path());
        CHECK(read.scanMaxResults == defaults.scanMaxResults);
        CHECK(read.showLogs == defaults.showLogs);
    }
    SECTION("an empty file") {
        file.write("");
        CHECK(infra::loadSettings(file.path()).scanMaxResults == defaults.scanMaxResults);
    }
    SECTION("something that is not a settings file") {
        file.write("<html><body>not this</body></html>\n");
        CHECK(infra::loadSettings(file.path()).scanMaxResults == defaults.scanMaxResults);
    }
    SECTION("a header from a version that does not exist yet") {
        file.write("POINTERLAB-SETTINGS 99\nscan.max_results=1\n");
        // Refused wholesale: a newer file may mean something different by the
        // same key, and guessing is worse than starting from defaults.
        CHECK(infra::loadSettings(file.path()).scanMaxResults == defaults.scanMaxResults);
    }
    SECTION("binary rubbish") {
        file.write(std::string("\x00\x01\x02\xFF garbage", 12));
        CHECK(infra::loadSettings(file.path()).scanMaxResults == defaults.scanMaxResults);
    }
}

TEST_CASE("One bad line does not discard the rest of the file", "[settings]") {
    TempSettings file;
    file.write("POINTERLAB-SETTINGS 1\n"
               "# a comment\n"
               "\n"
               "scan.max_results=4096\n"
               "scan.float_epsilon=not a number\n"      // unparseable: keeps its default
               "panel.logs=maybe\n"                     // not a boolean: keeps its default
               "a line with no separator\n"
               "future.setting=17\n"                    // from a newer version: ignored
               "panel.injection=false\n");

    const auto read = infra::loadSettings(file.path());
    const infra::Settings defaults;

    CHECK(read.scanMaxResults == 4096);
    CHECK_FALSE(read.showInjection);
    // The two rejected values, and only those, stayed at their defaults.
    CHECK(read.scanFloatEpsilon == defaults.scanFloatEpsilon);
    CHECK(read.showLogs == defaults.showLogs);
}

TEST_CASE("A BOM and CRLF line endings are tolerated", "[settings]") {
    TempSettings file;
    // What a user gets for editing the file in Notepad or PowerShell.
    file.write("\xEF\xBB\xBF" "POINTERLAB-SETTINGS 1\r\nscan.max_results=7777\r\npanel.modules=false\r\n");

    const auto read = infra::loadSettings(file.path());
    CHECK(read.scanMaxResults == 7777);
    CHECK_FALSE(read.showModules);
}

TEST_CASE("Saving creates the directory it needs", "[settings]") {
    const auto directory = std::filesystem::temp_directory_path() / "pointerlab_settings_nested_test";
    std::error_code code;
    std::filesystem::remove_all(directory, code);

    const auto path = directory / "nested" / "settings.ini";
    infra::Settings settings;
    settings.scanMaxResults = 31337;

    REQUIRE(infra::saveSettings(path, settings));
    CHECK(infra::loadSettings(path).scanMaxResults == 31337);

    std::filesystem::remove_all(directory, code);
}
