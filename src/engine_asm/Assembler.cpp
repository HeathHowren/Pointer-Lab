#include "engine_asm/Assembler.h"

#include "domain/Domain.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ire::engine_asm {

namespace {

std::string trim(std::string text) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), text.end());
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::uintptr_t parseInteger(const std::string& text) {
    return static_cast<std::uintptr_t>(std::stoull(text, nullptr, 0));
}

template <typename T>
void append(std::vector<std::uint8_t>& out, T value) {
    auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

} // namespace

infra::Result<std::vector<std::uint8_t>> Assembler::assemble(const std::string& source, std::uintptr_t baseAddress) const {
    std::vector<std::uint8_t> bytes;
    std::istringstream input(source);
    std::string line;

    try {
        while (std::getline(input, line)) {
            if (const auto comment = line.find(';'); comment != std::string::npos) {
                line = line.substr(0, comment);
            }
            line = trim(line);
            if (line.empty()) {
                continue;
            }
            const auto normalized = lower(line);
            const auto currentAddress = baseAddress + bytes.size();

            if (normalized == "nop") {
                bytes.push_back(0x90);
            } else if (normalized == "ret") {
                bytes.push_back(0xC3);
            } else if (normalized == "int3" || normalized == "break") {
                bytes.push_back(0xCC);
            } else if (normalized == "xor eax,eax" || normalized == "xor eax, eax") {
                bytes.insert(bytes.end(), {0x31, 0xC0});
            } else if (normalized.rfind("db ", 0) == 0) {
                auto raw = domain::parseHexBytes(line.substr(3));
                bytes.insert(bytes.end(), raw.begin(), raw.end());
            } else if (normalized.rfind("jmp ", 0) == 0) {
                const auto target = parseInteger(trim(line.substr(4)));
                const auto rel = static_cast<std::int32_t>(target - (currentAddress + 5));
                bytes.push_back(0xE9);
                append(bytes, rel);
            } else if (normalized.rfind("call ", 0) == 0) {
                const auto target = parseInteger(trim(line.substr(5)));
                const auto rel = static_cast<std::int32_t>(target - (currentAddress + 5));
                bytes.push_back(0xE8);
                append(bytes, rel);
            } else if (normalized.rfind("push ", 0) == 0) {
                const auto value = static_cast<std::uint32_t>(parseInteger(trim(line.substr(5))));
                bytes.push_back(0x68);
                append(bytes, value);
            } else if (normalized.rfind("mov rax,", 0) == 0) {
                const auto value = static_cast<std::uint64_t>(parseInteger(trim(line.substr(line.find(',') + 1))));
                bytes.insert(bytes.end(), {0x48, 0xB8});
                append(bytes, value);
            } else {
                return infra::Result<std::vector<std::uint8_t>>::fail("Unsupported assembly line: " + line);
            }
        }
    } catch (const std::exception& e) {
        return infra::Result<std::vector<std::uint8_t>>::fail(e.what());
    }

    return infra::Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

} // namespace ire::engine_asm

