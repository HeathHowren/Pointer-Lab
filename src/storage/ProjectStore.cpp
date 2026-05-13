#include "storage/ProjectStore.h"

#include <fstream>
#include <sstream>

namespace ire::storage {

namespace {

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

std::uintptr_t parseHexAddress(const std::string& text) {
    return static_cast<std::uintptr_t>(std::stoull(text, nullptr, 16));
}

} // namespace

infra::Result<void> ProjectStore::save(const std::filesystem::path& path, const ProjectTable& table) const {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return infra::Result<void>::fail("Could not open project file for writing.");
    }

    out << "IRETABLE 1\n";
    out << "pid|" << table.lastPid << "\n";
    out << "process|" << escape(domain::narrow(table.lastProcessName)) << "\n";
    for (const auto& entry : table.entries) {
        out << "entry|"
            << entry.id << '|'
            << std::hex << entry.address << std::dec << '|'
            << domain::valueTypeName(entry.type) << '|'
            << (entry.frozen ? 1 : 0) << '|'
            << escape(entry.description) << '|'
            << escape(entry.group) << '|'
            << escape(entry.hotkey) << '|'
            << domain::bytesToHex(entry.frozenValue, false) << "\n";
    }
    return infra::Result<void>::ok();
}

infra::Result<ProjectTable> ProjectStore::load(const std::filesystem::path& path) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return infra::Result<ProjectTable>::fail("Could not open project file.");
    }

    std::string line;
    std::getline(in, line);
    if (line != "IRETABLE 1") {
        return infra::Result<ProjectTable>::fail("Unsupported project file.");
    }

    ProjectTable table;
    while (std::getline(in, line)) {
        auto parts = splitEscaped(line);
        if (parts.empty()) {
            continue;
        }
        if (parts[0] == "pid" && parts.size() >= 2) {
            table.lastPid = static_cast<std::uint32_t>(std::stoul(parts[1]));
        } else if (parts[0] == "process" && parts.size() >= 2) {
            table.lastProcessName = domain::widen(parts[1]);
        } else if (parts[0] == "entry" && parts.size() >= 9) {
            domain::AddressEntry entry;
            entry.id = std::stoull(parts[1]);
            entry.address = parseHexAddress(parts[2]);
            if (auto type = domain::parseValueType(parts[3])) {
                entry.type = *type;
            }
            entry.frozen = parts[4] == "1";
            entry.description = parts[5];
            entry.group = parts[6];
            entry.hotkey = parts[7];
            entry.frozenValue = domain::parseHexBytes(parts[8]);
            table.entries.push_back(std::move(entry));
        }
    }

    return infra::Result<ProjectTable>::ok(std::move(table));
}

} // namespace ire::storage

