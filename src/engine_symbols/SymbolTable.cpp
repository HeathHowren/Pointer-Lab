#include "engine_symbols/SymbolTable.h"

#include <algorithm>
#include <cctype>

namespace ire::engine_symbols {

namespace {

using Address = infra::Result<std::uintptr_t>;

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool sameName(const std::string& a, const std::string& b) {
    return lower(a) == lower(b);
}

// One `+hex` or `-hex` term.
struct Term {
    bool subtract{};
    std::string text;
};

// Splits "client.dll+0x4A-8" into "client.dll" and {+0x4A, -8}.
//
// Only '+' and '-' separate terms, and the leading term is taken whole. A
// module name may contain almost anything else -- dots, underscores, digits --
// so anything cleverer would have to know what a module name looks like, and
// the answer to that is "whatever the loader says".
bool split(const std::string& expression, std::string& head, std::vector<Term>& terms) {
    const auto start = expression.find_first_of("+-");
    if (start == std::string::npos) {
        head = trim(expression);
        return !head.empty();
    }
    head = trim(expression.substr(0, start));
    if (head.empty()) {
        return false;
    }
    for (std::size_t i = start; i < expression.size();) {
        const bool subtract = expression[i] == '-';
        ++i;
        const auto next = expression.find_first_of("+-", i);
        const auto length = next == std::string::npos ? std::string::npos : next - i;
        auto text = trim(expression.substr(i, length));
        if (text.empty()) {
            return false;
        }
        terms.push_back({subtract, std::move(text)});
        i = next == std::string::npos ? expression.size() : next;
    }
    return true;
}

} // namespace

Address SymbolTable::resolveTerm(domain::TargetSession& session, const std::string& term) const {
    {
        std::scoped_lock lock(mutex_);
        const auto symbol = std::find_if(symbols_.begin(), symbols_.end(),
                                         [&term](const Symbol& s) { return sameName(s.name, term); });
        if (symbol != symbols_.end()) {
            return Address::ok(symbol->address);
        }
    }

    if (!session.attached()) {
        // A bare address still resolves without a target; anything naming a
        // module cannot, and saying so beats "not a valid address".
        if (auto address = domain::parseAddress(term)) {
            return Address::ok(*address);
        }
        return Address::fail("\"" + term + "\" names something in the target, but nothing is attached.");
    }

    // A whole module: "client.dll" is its base address.
    if (const auto base = ExportResolver::moduleBase(session, domain::widen(term)); base != 0) {
        return Address::ok(base);
    }

    // module.export / module!export. Split at the last separator so that
    // "kernel32.dll.LoadLibraryW" splits into the module and the function
    // rather than into "kernel32" and "dll.LoadLibraryW".
    const auto separator = term.find_last_of(".!");
    if (separator != std::string::npos && separator + 1 < term.size()) {
        const auto moduleName = term.substr(0, separator);
        const auto exportName = term.substr(separator + 1);
        if (const auto base = ExportResolver::moduleBase(session, domain::widen(moduleName)); base != 0) {
            auto resolved = exports_.resolve(session, domain::widen(moduleName), exportName);
            if (resolved) {
                return resolved;
            }
            return Address::fail(moduleName + " is loaded, but exports no \"" + exportName + "\": " +
                                 resolved.error());
        }
    }

    if (auto address = domain::parseAddress(term)) {
        return Address::ok(*address);
    }
    return Address::fail("\"" + term + "\" is not a hexadecimal address, a loaded module, an export of one, or a "
                                       "symbol you have defined.");
}

Address SymbolTable::resolve(domain::TargetSession& session, const std::string& expression) const {
    std::string head;
    std::vector<Term> terms;
    if (!split(expression, head, terms)) {
        return Address::fail("Enter an address, or a name such as client.dll+0x4A2C10.");
    }

    auto base = resolveTerm(session, head);
    if (!base) {
        return base;
    }

    std::uintptr_t address = base.value();
    for (const auto& term : terms) {
        // Offsets are hexadecimal, like every other number in this tool. An
        // offset that read as decimal because it happened to have no letters in
        // it would be a silent wrong answer, which is the worst kind.
        const auto offset = domain::parseAddress(term.text);
        if (!offset) {
            return Address::fail("\"" + term.text + "\" is not a hexadecimal offset.");
        }
        address = term.subtract ? address - *offset : address + *offset;
    }
    return Address::ok(address);
}

Address SymbolTable::define(domain::TargetSession& session, std::string name, const std::string& expression) {
    auto address = resolve(session, expression);
    if (!address) {
        return address;
    }
    const auto trimmedName = trim(name);
    if (trimmedName.empty()) {
        return Address::fail("A symbol needs a name.");
    }
    // A name that parses as an address would shadow that address everywhere,
    // and a name containing an operator could never be typed back in.
    if (domain::parseAddress(trimmedName).has_value()) {
        return Address::fail("\"" + trimmedName +
                             "\" reads as a hexadecimal address, so it cannot also be a symbol name.");
    }
    if (trimmedName.find_first_of("+-") != std::string::npos) {
        return Address::fail("A symbol name cannot contain '+' or '-'; those separate the offsets.");
    }

    std::scoped_lock lock(mutex_);
    const auto existing = std::find_if(symbols_.begin(), symbols_.end(),
                                       [&trimmedName](const Symbol& s) { return sameName(s.name, trimmedName); });
    if (existing != symbols_.end()) {
        existing->address = address.value();
        existing->expression = expression;
    } else {
        symbols_.push_back({trimmedName, address.value(), expression});
    }
    return address;
}

infra::Result<void> SymbolTable::define(std::string name, std::uintptr_t address) {
    const auto trimmedName = trim(name);
    if (trimmedName.empty()) {
        return infra::Result<void>::fail("A symbol needs a name.");
    }
    if (domain::parseAddress(trimmedName).has_value()) {
        return infra::Result<void>::fail("\"" + trimmedName +
                                         "\" reads as a hexadecimal address, so it cannot also be a symbol name.");
    }
    if (trimmedName.find_first_of("+-") != std::string::npos) {
        return infra::Result<void>::fail("A symbol name cannot contain '+' or '-'; those separate the offsets.");
    }

    std::scoped_lock lock(mutex_);
    const auto existing = std::find_if(symbols_.begin(), symbols_.end(),
                                       [&trimmedName](const Symbol& s) { return sameName(s.name, trimmedName); });
    if (existing != symbols_.end()) {
        existing->address = address;
        existing->expression.clear();
    } else {
        symbols_.push_back({trimmedName, address, {}});
    }
    return infra::Result<void>::ok();
}

bool SymbolTable::undefine(const std::string& name) {
    std::scoped_lock lock(mutex_);
    const auto removed = std::remove_if(symbols_.begin(), symbols_.end(),
                                        [&name](const Symbol& s) { return sameName(s.name, name); });
    if (removed == symbols_.end()) {
        return false;
    }
    symbols_.erase(removed, symbols_.end());
    return true;
}

void SymbolTable::clear() {
    std::scoped_lock lock(mutex_);
    symbols_.clear();
}

std::vector<Symbol> SymbolTable::symbols() const {
    std::scoped_lock lock(mutex_);
    auto copy = symbols_;
    std::sort(copy.begin(), copy.end(),
              [](const Symbol& a, const Symbol& b) { return lower(a.name) < lower(b.name); });
    return copy;
}

std::optional<Symbol> SymbolTable::find(const std::string& name) const {
    std::scoped_lock lock(mutex_);
    const auto symbol = std::find_if(symbols_.begin(), symbols_.end(),
                                     [&name](const Symbol& s) { return sameName(s.name, name); });
    if (symbol == symbols_.end()) {
        return std::nullopt;
    }
    return *symbol;
}

std::optional<domain::ModuleInfo> SymbolTable::moduleAt(domain::TargetSession& session, std::uintptr_t address) {
    if (!session.attached() || address == 0) {
        return std::nullopt;
    }
    const auto modules = session.modules();
    const auto module = std::find_if(modules.begin(), modules.end(), [address](const domain::ModuleInfo& m) {
        return address >= m.base && address < m.base + m.size;
    });
    if (module == modules.end()) {
        return std::nullopt;
    }
    return *module;
}

bool SymbolTable::isStatic(domain::TargetSession& session, std::uintptr_t address) {
    return moduleAt(session, address).has_value();
}

std::string SymbolTable::describe(domain::TargetSession& session, std::uintptr_t address) const {
    {
        std::scoped_lock lock(mutex_);
        const auto symbol = std::find_if(symbols_.begin(), symbols_.end(),
                                         [address](const Symbol& s) { return s.address == address; });
        if (symbol != symbols_.end()) {
            return symbol->name;
        }
    }

    const auto module = moduleAt(session, address);
    if (!module) {
        return {};
    }
    const auto offset = address - module->base;
    return domain::narrow(module->name) + (offset == 0 ? "" : "+" + domain::toHex(offset));
}

std::string SymbolTable::describe(const std::vector<domain::ModuleInfo>& modules, std::uintptr_t address) const {
    if (address == 0) {
        return {};
    }
    {
        std::scoped_lock lock(mutex_);
        const auto symbol = std::find_if(symbols_.begin(), symbols_.end(),
                                         [address](const Symbol& s) { return s.address == address; });
        if (symbol != symbols_.end()) {
            return symbol->name;
        }
    }

    const auto module = std::find_if(modules.begin(), modules.end(), [address](const domain::ModuleInfo& m) {
        return address >= m.base && address < m.base + m.size;
    });
    if (module == modules.end()) {
        return {};
    }
    const auto offset = address - module->base;
    return domain::narrow(module->name) + (offset == 0 ? "" : "+" + domain::toHex(offset));
}

} // namespace ire::engine_symbols
