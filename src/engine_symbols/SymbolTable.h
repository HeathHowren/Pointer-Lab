#pragma once

#include "domain/TargetSession.h"
#include "engine_symbols/ExportResolver.h"
#include "infra/Result.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ire::engine_symbols {

struct Symbol {
    std::string name;
    std::uintptr_t address{};
    // What the user typed, when the symbol was defined from an expression.
    // Kept so the definition can be shown and re-resolved after a restart
    // rather than being frozen at whatever the address happened to be.
    std::string expression;
};

// Names for addresses, in both directions.
//
// Typing a bare hexadecimal address everywhere is not merely inconvenient: it
// is the reason a session's work evaporates when the target restarts. ASLR
// moves every module, so the address that was the answer yesterday is a
// different thing today, whereas `client.dll+0x4A2C10` is the same thing every
// run. Every address box in Pointer Lab accepts these expressions, so a chain
// written down once keeps working.
//
// Going the other way, `describe()` is what lets the scanner mark a result as
// static -- green in the results table -- which is the check a reader uses to
// tell "this address will still be here next launch" from "this address is
// wherever the allocator happened to put it".
class SymbolTable {
public:
    // Accepted forms, tried in this order:
    //
    //   <user symbol>          a name defined with define()
    //   <module>               the base address of a loaded module
    //   <module>+<hex>         module base plus an offset -- the usual form
    //   <module>.<export>      an exported function, resolved out of the target
    //   <module>!<export>      the same, in debugger notation
    //   <hex>                  a plain address, with or without 0x
    //
    // and any of them followed by any number of `+hex` / `-hex` terms.
    //
    // The error names which part failed, because "not a valid address" for a
    // typed module name that is simply not loaded yet sends people looking in
    // the wrong place.
    infra::Result<std::uintptr_t> resolve(domain::TargetSession& session, const std::string& expression) const;

    // Defines a user symbol. The expression is resolved now to check it, and
    // kept so the symbol re-resolves against the current target rather than
    // holding an address from a previous run.
    infra::Result<std::uintptr_t> define(domain::TargetSession& session, std::string name,
                                         const std::string& expression);
    // Defines a symbol for a fixed address, for callers that already have one.
    infra::Result<void> define(std::string name, std::uintptr_t address);
    bool undefine(const std::string& name);
    void clear();

    [[nodiscard]] std::vector<Symbol> symbols() const;
    [[nodiscard]] std::optional<Symbol> find(const std::string& name) const;

    // The most specific name for an address: a user symbol that names it
    // exactly, else `module.dll+0x1234` when it is inside a loaded module, else
    // empty. Never invents a name for a heap address.
    [[nodiscard]] std::string describe(domain::TargetSession& session, std::uintptr_t address) const;

    // True when the address is inside a loaded module's image. Such an address
    // is at the same module+offset in every run, which is exactly what makes it
    // worth writing down.
    [[nodiscard]] static bool isStatic(domain::TargetSession& session, std::uintptr_t address);
    [[nodiscard]] static std::optional<domain::ModuleInfo> moduleAt(domain::TargetSession& session,
                                                                    std::uintptr_t address);

private:
    // Resolves the leading term, with the +/- offsets already stripped.
    infra::Result<std::uintptr_t> resolveTerm(domain::TargetSession& session, const std::string& term) const;

    ExportResolver exports_;
    mutable std::mutex mutex_;
    std::vector<Symbol> symbols_;
};

} // namespace ire::engine_symbols
