#include "storage/ProjectStore.h"

#include "infra/Logger.h"

#include <cctype>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>

namespace ire::storage {

namespace {

// 1 -> 2 when the reader learned to tolerate extra trailing fields.
// 2 -> 3 added the three pointer-chain fields at the end of an entry row. Both
// older versions still load: a row without those fields is simply a fixed
// address, which is what it always was.
constexpr int currentFormatVersion = 3;

std::string formatOffsets(const std::vector<std::ptrdiff_t>& offsets) {
    std::ostringstream out;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        // The unsigned bit pattern, read back with the matching cast. A manually
        // entered chain may step backwards through a structure, and streaming a
        // negative signed value writes a minus sign that only round-trips
        // through stoull's wrap-on-negate -- which works, but by accident.
        out << std::hex << static_cast<std::uintptr_t>(offsets[i]);
    }
    return out.str();
}

std::optional<std::vector<std::ptrdiff_t>> parseOffsets(const std::string& text) {
    std::vector<std::ptrdiff_t> offsets;
    std::istringstream in(text);
    std::string field;
    while (std::getline(in, field, ',')) {
        if (field.empty()) {
            return std::nullopt;
        }
        // Offsets are written unsigned in hex; they are never negative in a
        // chain produced by the scanner.
        std::size_t consumed{};
        try {
            const auto value = std::stoull(field, &consumed, 16);
            if (consumed != field.size()) {
                return std::nullopt;
            }
            offsets.push_back(static_cast<std::ptrdiff_t>(value));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return offsets;
}

std::string escape(std::string text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '|' || c == '\n' || c == '\r') {
            out.push_back('\\');
            if (c == '\n') out.push_back('n');
            else if (c == '\r') out.push_back('r');
            else out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string unescape(std::string text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char n = text[++i];
            if (n == 'n') out.push_back('\n');
            else if (n == 'r') out.push_back('\r');
            else out.push_back(n);
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::vector<std::string> splitEscaped(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    bool escaped{};
    for (const char c : line) {
        if (escaped) {
            current.push_back('\\');
            current.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '|') {
            parts.push_back(unescape(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(unescape(current));
    return parts;
}

// Every numeric field in a project file is attacker-controllable in the sense
// that the user can hand-edit it or receive it from someone else. The bare
// std::stoull calls these replace threw straight out of load(), and because an
// uncaught C++ exception bypasses the SEH crash filter it produced no crash log
// either.
std::optional<std::uint64_t> parseUnsigned(const std::string& text, int base) {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    const auto isValidDigit = [base](unsigned char c) {
        return base == 16 ? std::isxdigit(c) != 0 : std::isdigit(c) != 0;
    };
    for (const char c : text) {
        if (!isValidDigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }
    try {
        return std::stoull(text, nullptr, base);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

infra::Result<void> ProjectStore::save(const std::filesystem::path& path, const ProjectTable& table) const {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return infra::Result<void>::fail("Could not open project file for writing.");
    }

    out << "IRETABLE " << currentFormatVersion << "\n";
    out << "pid|" << table.lastPid << "\n";
    out << "process|" << escape(domain::narrow(table.lastProcessName)) << "\n";
    // A new record rather than a format version bump. Unrecognised record types
    // are ignored by every reader this format has ever had, so a 2.1.0 build
    // loads this file and simply skips the line -- whereas "IRETABLE 4" would
    // make it refuse the whole table.
    out << "bitness|" << (table.lastBitness == domain::Bitness::X86 ? "x86" : "x64") << "\n";
    // Same reasoning: a new record type, no version bump.
    for (const auto& symbol : table.symbols) {
        out << "symbol|" << escape(symbol.name) << '|' << escape(symbol.expression) << "\n";
    }
    // And again. A script's newlines survive because escape() turns them into
    // \n, so the whole source is one line of the file however long it is.
    for (const auto& script : table.scripts) {
        out << "script|" << escape(script.name) << '|' << escape(script.source) << "\n";
    }
    // A structure is a header line followed by one line per field, in offset
    // order. Fields could have been packed into the header, but a layout is the
    // record in this file most likely to be read and edited by hand, and one
    // field per line is what makes that bearable.
    for (const auto& structure : table.structures) {
        out << "struct|" << escape(structure.name) << "\n";
        for (const auto& field : structure.fields) {
            out << "field|" << std::hex << static_cast<std::uintptr_t>(field.offset) << std::dec << '|'
                << domain::valueTypeName(field.type) << '|' << field.length << '|' << escape(field.name)
                << "\n";
        }
    }
    for (const auto& entry : table.entries) {
        out << "entry|"
            << entry.id << '|'
            << std::hex << entry.address << std::dec << '|'
            << domain::valueTypeName(entry.type) << '|'
            << (entry.frozen ? 1 : 0) << '|'
            << escape(entry.description) << '|'
            << escape(entry.group) << '|'
            << escape(entry.hotkey) << '|'
            << domain::bytesToHex(entry.frozenValue, false) << '|'
            // Pointer chain, or three empty fields for a plain fixed address.
            << (entry.chain ? escape(domain::narrow(entry.chain->moduleName)) : "") << '|'
            << std::hex << (entry.chain ? entry.chain->moduleOffset : 0) << std::dec << '|'
            << (entry.chain ? formatOffsets(entry.chain->offsets) : "") << "\n";
    }

    out.flush();
    if (!out) {
        return infra::Result<void>::fail("Could not write the project file (the disk may be full or read-only).");
    }
    infra::Logger::instance().info("Saved " + std::to_string(table.entries.size()) + " entries to " + path.string());
    return infra::Result<void>::ok();
}

infra::Result<ProjectTable> ProjectStore::load(const std::filesystem::path& path) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return infra::Result<ProjectTable>::fail("Could not open project file.");
    }

    std::string line;
    std::getline(in, line);
    // Text editors (and PowerShell) happily add a UTF-8 BOM, which would
    // otherwise make the header compare fail and the file look corrupt.
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    // Version 1 files are still readable; unknown newer versions are refused
    // rather than silently misparsed.
    if (line != "IRETABLE 1" && line != "IRETABLE 2" && line != "IRETABLE 3") {
        return infra::Result<ProjectTable>::fail(
            "This is not a Pointer Lab project file, or it was written by a newer version.");
    }

    ProjectTable table;
    std::size_t lineNumber = 1;
    std::size_t skipped = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto parts = splitEscaped(line);
        if (parts.empty()) {
            continue;
        }

        if (parts[0] == "pid" && parts.size() >= 2) {
            if (auto pid = parseUnsigned(parts[1], 10)) {
                table.lastPid = static_cast<std::uint32_t>(*pid);
            } else {
                infra::Logger::instance().warn("Unreadable process id on line " + std::to_string(lineNumber) +
                                               "; ignoring it.");
                ++skipped;
            }
        } else if (parts[0] == "process" && parts.size() >= 2) {
            table.lastProcessName = domain::widen(parts[1]);
        } else if (parts[0] == "bitness" && parts.size() >= 2) {
            // Absent in files written before this record existed, which is why
            // the default is x64 rather than an error: every such file was
            // written by a build that could only attach to 64-bit targets.
            if (parts[1] == "x86") {
                table.lastBitness = domain::Bitness::X86;
            } else if (parts[1] == "x64") {
                table.lastBitness = domain::Bitness::X64;
            } else {
                infra::Logger::instance().warn("Unrecognised target bitness \"" + parts[1] + "\" on line " +
                                               std::to_string(lineNumber) + "; assuming 64-bit.");
                ++skipped;
            }
        } else if (parts[0] == "symbol" && parts.size() >= 3) {
            if (parts[1].empty() || parts[2].empty()) {
                infra::Logger::instance().warn("Incomplete symbol on line " + std::to_string(lineNumber) +
                                               "; ignoring it.");
                ++skipped;
            } else {
                table.symbols.push_back({parts[1], parts[2]});
            }
        } else if (parts[0] == "script" && parts.size() >= 3) {
            // An empty source is kept: it is a script the user started and has
            // not written yet, and losing it on save/load would be worse than
            // carrying a blank one.
            if (parts[1].empty() && parts[2].empty()) {
                infra::Logger::instance().warn("Empty script on line " + std::to_string(lineNumber) +
                                               "; ignoring it.");
                ++skipped;
            } else {
                table.scripts.push_back({parts[1], parts[2]});
            }
        } else if (parts[0] == "struct" && parts.size() >= 2) {
            domain::Structure structure;
            structure.id = table.structures.size() + 1;
            structure.name = parts[1].empty() ? "Structure " + std::to_string(structure.id) : parts[1];
            table.structures.push_back(std::move(structure));
        } else if (parts[0] == "field" && parts.size() >= 5) {
            // Fields belong to the structure above them. A field with nothing
            // above it is named rather than silently attached to whatever comes
            // next, because a layout in the wrong structure is worse than a
            // missing one.
            if (table.structures.empty()) {
                infra::Logger::instance().warn("Field on line " + std::to_string(lineNumber) +
                                               " comes before any struct record; ignoring it.");
                ++skipped;
                continue;
            }
            const auto offset = parseUnsigned(parts[1], 16);
            const auto type = domain::parseValueType(parts[2]);
            const auto length = parseUnsigned(parts[3], 10);
            if (!offset || !type || !length) {
                infra::Logger::instance().warn("Unreadable field on line " + std::to_string(lineNumber) +
                                               "; skipping it.");
                ++skipped;
                continue;
            }
            domain::StructureField field;
            // Written as the unsigned bit pattern, like a chain offset, so a
            // field above the start of the object round-trips exactly.
            field.offset = static_cast<std::ptrdiff_t>(static_cast<std::uintptr_t>(*offset));
            field.type = *type;
            field.length = static_cast<std::size_t>(*length);
            field.name = parts[4].empty() ? domain::defaultFieldName(field.offset) : parts[4];
            if (field.size() == 0) {
                infra::Logger::instance().warn("Field \"" + field.name + "\" on line " +
                                               std::to_string(lineNumber) +
                                               " has no width; skipping it.");
                ++skipped;
                continue;
            }
            table.structures.back().fields.push_back(std::move(field));
        } else if (parts[0] == "entry" && parts.size() >= 9) {
            const auto id = parseUnsigned(parts[1], 10);
            const auto address = parseUnsigned(parts[2], 16);
            if (!id || !address) {
                // One corrupt row must not cost the user the whole table.
                infra::Logger::instance().warn("Unreadable id or address on line " + std::to_string(lineNumber) +
                                               "; skipping that entry.");
                ++skipped;
                continue;
            }
            domain::AddressEntry entry;
            entry.id = *id;
            entry.address = static_cast<std::uintptr_t>(*address);
            if (auto type = domain::parseValueType(parts[3])) {
                entry.type = *type;
            } else {
                // Silently keeping the default type would give the entry the
                // wrong width and read/write the wrong number of bytes.
                infra::Logger::instance().warn(
                    "Unknown value type \"" + parts[3] + "\" on line " + std::to_string(lineNumber) +
                    "; skipping that entry.");
                ++skipped;
                continue;
            }
            entry.frozen = parts[4] == "1";
            entry.description = parts[5];
            entry.group = parts[6];
            entry.hotkey = parts[7];
            entry.frozenValue = domain::parseHexBytes(parts[8]);

            // Pointer chain, present from version 3. All three fields empty --
            // no module, a zero base and no offsets -- means a plain fixed
            // address, which is what versions 1 and 2 had. A chain with no
            // module name but a base is a manually entered one rooted at an
            // absolute address; it is legal, and does not survive a restart.
            if (parts.size() >= 12 && (!parts[9].empty() || parseUnsigned(parts[10], 16).value_or(0) != 0)) {
                const auto moduleOffset = parseUnsigned(parts[10], 16);
                const auto offsets = parseOffsets(parts[11]);
                if (!moduleOffset || !offsets) {
                    infra::Logger::instance().warn(
                        "Malformed pointer chain on line " + std::to_string(lineNumber) +
                        "; keeping the entry as a fixed address.");
                    ++skipped;
                } else {
                    domain::PointerChain chain;
                    chain.moduleName = domain::widen(parts[9]);
                    chain.moduleOffset = static_cast<std::uintptr_t>(*moduleOffset);
                    chain.offsets = *offsets;
                    // The saved address was resolved in a process that has since
                    // exited, so the entry starts out unresolved and the
                    // background pass recomputes it against the new target.
                    entry.resolved = false;
                    entry.chain = std::move(chain);
                }
            }

            table.entries.push_back(std::move(entry));
        } else {
            // Too few fields, or a record type this version does not know.
            // Named rather than only counted, because the person most likely to
            // hit this is the one hand-editing the file.
            infra::Logger::instance().warn("Unrecognised or incomplete record on line " +
                                           std::to_string(lineNumber) + "; ignoring it.");
            ++skipped;
        }
    }

    if (skipped > 0) {
        infra::Logger::instance().warn(
            "Skipped " + std::to_string(skipped) + " unreadable line(s) while loading " + path.filename().string() + ".");
    }
    infra::Logger::instance().info("Loaded " + std::to_string(table.entries.size()) + " entries from " + path.string());
    return infra::Result<ProjectTable>::ok(std::move(table));
}

} // namespace ire::storage

