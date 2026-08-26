#include "engine_asm/Assembler.h"

#include <keystone/keystone.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ire::engine_asm {

namespace {

using Bytes = infra::Result<std::vector<std::uint8_t>>;

std::string trim(std::string text) {
    const auto notSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

// Strips ';' and '//' comments and blank lines. Keystone treats ';' as a
// statement separator, so without this it would try to assemble comment text as
// code and report a confusing error on a line that looks perfectly fine.
std::string stripComments(const std::string& source) {
    std::istringstream input(source);
    std::string line;
    std::string result;

    while (std::getline(input, line)) {
        if (const auto comment = line.find(';'); comment != std::string::npos) {
            line.erase(comment);
        }
        if (const auto comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (!result.empty()) {
            result.push_back('\n');
        }
        result += line;
    }
    return result;
}

// RAII around the engine handle; ks_asm has several failure paths and each one
// would otherwise need its own ks_close.
class Engine {
public:
    explicit Engine(domain::Bitness bitness) {
        error_ = ks_open(KS_ARCH_X86, bitness == domain::Bitness::X86 ? KS_MODE_32 : KS_MODE_64, &handle_);
        if (error_ == KS_ERR_OK && handle_ != nullptr) {
            error_ = ks_option(handle_, KS_OPT_SYNTAX, KS_OPT_SYNTAX_INTEL);
        }
    }

    ~Engine() {
        if (handle_ != nullptr) {
            ks_close(handle_);
        }
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] bool ok() const { return error_ == KS_ERR_OK && handle_ != nullptr; }
    [[nodiscard]] ks_engine* get() const { return handle_; }
    [[nodiscard]] ks_err error() const { return error_; }

private:
    ks_engine* handle_{};
    ks_err error_{KS_ERR_OK};
};

} // namespace

infra::Result<std::vector<std::uint8_t>> Assembler::assemble(const std::string& source, std::uintptr_t baseAddress,
                                                             domain::Bitness bitness) const {
    const auto text = stripComments(source);
    if (text.empty()) {
        return Bytes::fail("Nothing to assemble.");
    }

    Engine engine(bitness);
    if (!engine.ok()) {
        return Bytes::fail(std::string("Could not start the assembler: ") + ks_strerror(engine.error()));
    }

    unsigned char* encoding{};
    std::size_t encodingSize{};
    std::size_t statementCount{};
    // baseAddress matters: a relative jmp or call assembles to a different
    // displacement depending on where the patch will live.
    if (ks_asm(engine.get(), text.c_str(), static_cast<std::uint64_t>(baseAddress), &encoding, &encodingSize,
               &statementCount) != 0) {
        const auto code = ks_errno(engine.get());
        return Bytes::fail(std::string("Assembly failed: ") + ks_strerror(code),
                           static_cast<infra::ErrorCode>(code));
    }

    if (encoding == nullptr || encodingSize == 0) {
        if (encoding != nullptr) {
            ks_free(encoding);
        }
        return Bytes::fail("The assembler produced no bytes.");
    }

    std::vector<std::uint8_t> bytes(encoding, encoding + encodingSize);
    ks_free(encoding);
    return Bytes::ok(std::move(bytes));
}

} // namespace ire::engine_asm
