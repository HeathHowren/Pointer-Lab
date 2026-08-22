#include "infra/Settings.h"

#include "infra/Logger.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace ire::infra {

namespace {

// A flat key=value text file rather than JSON. The schema is two dozen scalars,
// so a nested format would buy nothing and cost a hand-written parser, and
// Pointer Lab already keeps its data in plain text a user can read and fix.
constexpr char settingsHeader[] = "POINTERLAB-SETTINGS 1";

// One list of fields, walked by both the reader and the writer, so the two
// cannot drift apart and quietly stop round-tripping a setting.
//
// Templated on the settings type so it binds `const Settings&` when saving and
// `Settings&` when loading, from a single definition of the list.
template <typename S, typename Fn>
void forEachField(S& settings, Fn&& field) {
    field("scan.max_results", settings.scanMaxResults);
    field("scan.float_epsilon", settings.scanFloatEpsilon);
    field("scan.writable_only", settings.scanWritableOnly);
    field("scan.executable_only", settings.scanExecutableOnly);
    field("scan.value_type", settings.scanTypeIndex);

    field("pointer.max_depth", settings.pointerDepth);
    field("pointer.value_type", settings.pointerTypeIndex);

    field("panel.memory_viewer", settings.showMemoryViewer);
    field("panel.disassembly", settings.showDisassembly);
    field("panel.breakpoints", settings.showBreakpoints);
    field("panel.modules", settings.showModules);
    field("panel.memory_regions", settings.showMemoryRegions);
    field("panel.logs", settings.showLogs);
    field("panel.pointer_scanner", settings.showPointerScanner);
    field("panel.lua_scanner", settings.showLuaScanner);
    field("panel.injection", settings.showInjection);
    field("panel.lua_console", settings.showLuaConsole);
}

std::string format(bool value) {
    return value ? "true" : "false";
}

std::string format(int value) {
    return std::to_string(value);
}

std::string format(std::uint64_t value) {
    return std::to_string(value);
}

std::string format(double value) {
    // Enough digits to come back as the same double; a tolerance that shifted
    // slightly on every save would be its own small bug.
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

// Each returns whether the text was usable. A rejected value leaves the field at
// its default rather than at something arbitrary.
bool parse(const std::string& text, bool& value) {
    if (text == "true" || text == "1") {
        value = true;
        return true;
    }
    if (text == "false" || text == "0") {
        value = false;
        return true;
    }
    return false;
}

bool parse(const std::string& text, int& value) {
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parse(const std::string& text, std::uint64_t& value) {
    if (text.empty() || text.front() == '-') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse(const std::string& text, double& value) {
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

// Text editors and PowerShell add a byte-order mark without asking, and one
// sitting in front of the header would otherwise make the file unreadable.
void stripBom(std::string& line) {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
}

void stripCarriageReturn(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

} // namespace

Settings loadSettings(const std::filesystem::path& path) {
    Settings settings;

    std::ifstream in(path);
    if (!in) {
        // No file yet is the normal first run, not a problem to report.
        return settings;
    }

    std::string line;
    if (!std::getline(in, line)) {
        return settings;
    }
    stripBom(line);
    stripCarriageReturn(line);
    if (line != settingsHeader) {
        Logger::instance().warn("Ignoring " + path.filename().string() +
                                ": it is not a Pointer Lab settings file, or was written by a newer version. "
                                "Defaults are in use and it will be overwritten on exit.");
        return settings;
    }

    std::size_t skipped = 0;
    while (std::getline(in, line)) {
        stripCarriageReturn(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            ++skipped;
            continue;
        }
        const auto key = line.substr(0, separator);
        const auto text = line.substr(separator + 1);

        bool matched = false;
        forEachField(settings, [&](const char* name, auto& value) {
            if (matched || key != name) {
                return;
            }
            matched = true;
            if (!parse(text, value)) {
                // Keep the default rather than a half-converted number.
                matched = false;
            }
        });
        if (!matched) {
            // An unknown key is not an error: it is how a file written by a
            // newer version stays readable by this one.
            ++skipped;
        }
    }

    if (skipped > 0) {
        Logger::instance().info("Loaded settings with " + std::to_string(skipped) +
                                " line(s) skipped as unrecognised.");
    }
    return settings;
}

bool saveSettings(const std::filesystem::path& path, const Settings& settings) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }

    out << settingsHeader << '\n';
    forEachField(settings, [&out](const char* name, const auto& value) { out << name << '=' << format(value) << '\n'; });
    out.flush();
    return static_cast<bool>(out);
}

} // namespace ire::infra
