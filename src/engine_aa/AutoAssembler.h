#pragma once

#include "domain/TargetSession.h"
#include "engine_asm/Assembler.h"
#include "engine_inject/Injector.h"
#include "engine_patch/PatchRegistry.h"
#include "engine_symbols/SymbolTable.h"
#include "infra/Result.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ire::engine_aa {

// One contiguous run of assembled bytes and where it goes.
struct Block {
    std::uintptr_t address{};
    std::vector<std::uint8_t> bytes;
    // True when the address is inside memory this script allocated, so the
    // bytes are being written into fresh pages rather than over the target's
    // own code. Those need no undo record: freeing the allocation is the undo.
    bool intoAllocation{};
};

struct Allocation {
    std::string name;
    std::uintptr_t address{};
    std::size_t size{};
};

// What a compile produced, before anything is written.
struct CompileResult {
    std::vector<Block> blocks;
    std::vector<Allocation> allocations;
    // Symbols the script asked to publish, with the values it computed.
    std::vector<std::pair<std::string, std::uintptr_t>> registerSymbols;
    std::vector<std::string> unregisterSymbols;
    // Names passed to dealloc().
    std::vector<std::string> deallocations;
    // Human-readable account of what the script worked out: where each scan
    // landed, what was allocated where. Shown after a successful enable,
    // because "it worked" is much less useful than "INJECT resolved to
    // ac_client.exe+0x8A3F1".
    std::vector<std::string> notes;
};

struct Script {
    std::uint64_t id{};
    std::string name;
    std::string source;
    bool enabled{};
};

// An auto-assembler in the tradition the book teaches: a script with an
// [ENABLE] section that patches the target and a [DISABLE] section that puts it
// back.
//
// The point of the format is that a code injection is a *description* rather
// than a sequence of manual steps. "Find these bytes, allocate a cave within
// reach of them, write this code into it, and jump to it from there" is one
// artefact that can be read, checked, shared and re-run after the target
// restarts -- which is not true of the same work done by hand in a hex editor.
//
// Directives, all of which take effect in the order they appear:
//
//   aobscanmodule(name, module, pattern)  find a byte pattern inside a module
//                                         and bind `name` to where it starts
//   alloc(name, size [, nearName])        allocate in the target; with a third
//                                         argument the block is placed within
//                                         2 GB of it so a five-byte jmp reaches
//   label(name)                           declare a label defined later
//   name:                                 define a label here, or, when `name`
//                                         already has a value, continue
//                                         assembling *at* that value
//   <address expression>:                 the same, for anything the symbol
//                                         table can resolve -- `7FF612340000:`,
//                                         `game.exe+8A3F1:`
//   registersymbol(name)                  publish to the global symbol table
//   unregistersymbol(name)                remove it again
//   dealloc(name)                         free an allocation
//   define(name, text)                    plain textual substitution
//   assert(name, bytes)                   refuse to run unless those bytes are
//                                         really there
//   db / dw / dd / dq                     raw data
//
// `assert` is the one that earns its place. A script written against one build
// of a game names addresses that mean something else in the next, and without
// the check the first thing it does after an update is overwrite the wrong
// instruction. Every generated template includes one.
class AutoAssembler {
public:
    AutoAssembler(domain::TargetSession& session, const engine_asm::Assembler& assembler,
                  engine_patch::PatchRegistry& patches, engine_symbols::SymbolTable& symbols,
                  engine_inject::Injector& injector);

    // Compiles the named section without writing anything or allocating
    // anything. For the editor's Check button: it reports what the script
    // *would* do, which is the only way to read a script critically before
    // running it. Scans and asserts do read the target.
    [[nodiscard]] infra::Result<CompileResult> check(const std::string& source, bool enableSection = true);

    std::uint64_t add(std::string name, std::string source);
    // Refuses while the script is enabled: the [DISABLE] section that would put
    // the target back is the one in the *old* source.
    infra::Result<void> update(std::uint64_t id, std::string name, std::string source);
    // Disables first, so removing a script never leaves its patches applied
    // with nothing left to undo them.
    infra::Result<void> remove(std::uint64_t id);
    infra::Result<void> setEnabled(std::uint64_t id, bool enabled);
    // Disables every enabled script, continuing past failures. For detaching in
    // an orderly way, and for closing a project.
    infra::Result<void> disableAll();
    void forgetAll();

    [[nodiscard]] std::vector<Script> scripts() const;
    [[nodiscard]] std::optional<Script> find(std::uint64_t id) const;
    // Notes from the last enable or disable of this script.
    [[nodiscard]] std::vector<std::string> lastNotes(std::uint64_t id) const;

    // The three shapes the book teaches, ready to edit. `address` is filled in
    // where the template needs one; `bytes` is the original instruction bytes
    // for the assert and the [DISABLE] section.
    enum class Template {
        AobInjection,
        CodeCave,
        FullInjection
    };
    [[nodiscard]] std::string makeTemplate(Template shape, std::uintptr_t address,
                                           const std::vector<std::uint8_t>& bytes,
                                           const std::string& moduleName) const;

    // How many layout passes are allowed before a script is declared
    // unresolvable. An instruction's length can depend on a label's value and a
    // label's value on that length, so the layout is iterated to a fixed point;
    // this bounds a pathological script that never settles.
    static constexpr int maxLayoutPasses = 8;

private:
    struct RunState {
        std::vector<std::uint64_t> patchIds;
        std::vector<Allocation> allocations;
        std::vector<std::string> registeredSymbols;
        std::vector<std::string> notes;
        // Every symbol the [ENABLE] run computed, not only the published ones.
        // The [DISABLE] section names them -- `INJECT:`, `newmem` -- and would
        // otherwise have nothing to resolve them against, since a script is not
        // obliged to registersymbol everything it uses.
        std::map<std::string, std::uintptr_t> symbols;
    };

    infra::Result<CompileResult> compile(const std::string& source, bool enableSection, bool execute,
                                         RunState* state);
    infra::Result<void> runEnable(Script& script, RunState& state);
    infra::Result<void> runDisable(Script& script, RunState& state);

    domain::TargetSession& session_;
    const engine_asm::Assembler& assembler_;
    engine_patch::PatchRegistry& patches_;
    engine_symbols::SymbolTable& symbols_;
    engine_inject::Injector& injector_;

    mutable std::mutex mutex_;
    std::vector<Script> scripts_;
    std::map<std::uint64_t, RunState> state_;
    std::uint64_t nextId_{1};
};

} // namespace ire::engine_aa
