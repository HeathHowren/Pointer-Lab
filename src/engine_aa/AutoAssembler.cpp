#include "engine_aa/AutoAssembler.h"

#include "engine_scan/MemoryScanner.h"
#include "infra/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ire::engine_aa {

namespace {

template <typename T>
using Result = infra::Result<T>;

// Symbol names are matched case-insensitively, like everything else that names
// something in this tool.
struct CaseLess {
    bool operator()(const std::string& a, const std::string& b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](unsigned char x, unsigned char y) {
                                                return std::tolower(x) < std::tolower(y);
                                            });
    }
};

using SymbolMap = std::map<std::string, std::uintptr_t, CaseLess>;
using DefineMap = std::map<std::string, std::string, CaseLess>;

struct Line {
    int number{};
    std::string text;
};

struct Directive {
    std::string name;
    std::vector<std::string> args;
};

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool identifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Strips comments and splits the source into its two sections. A source with no
// section markers at all is treated as one [ENABLE] section, because that is
// what a one-line experiment looks like and refusing it would be pedantry.
std::vector<Line> sectionLines(const std::string& source, bool enableSection) {
    std::vector<Line> lines;
    std::istringstream in(source);
    std::string raw;
    int number = 0;
    bool sawMarker = false;
    bool inSection = enableSection; // until a marker says otherwise

    while (std::getline(in, raw)) {
        ++number;
        // A ';' or "//" starts a comment. Neither can appear inside anything
        // this language has that would be harmed by cutting there.
        if (const auto semicolon = raw.find(';'); semicolon != std::string::npos) {
            raw.erase(semicolon);
        }
        if (const auto slashes = raw.find("//"); slashes != std::string::npos) {
            raw.erase(slashes);
        }
        auto text = trim(raw);
        if (text.empty()) {
            continue;
        }

        const auto marker = lower(text);
        if (marker == "[enable]") {
            sawMarker = true;
            inSection = enableSection;
            continue;
        }
        if (marker == "[disable]") {
            sawMarker = true;
            inSection = !enableSection;
            continue;
        }
        if (inSection) {
            lines.push_back({number, std::move(text)});
        }
    }

    if (!sawMarker && !enableSection) {
        // No [DISABLE] was written, so there is nothing for it to do. The patch
        // registry still restores what [ENABLE] wrote.
        return {};
    }
    return lines;
}

// `name(arg, arg)`. Returns nullopt for anything that is not shaped like a
// call, which is how instructions are told apart from directives.
std::optional<Directive> parseDirective(const std::string& text) {
    const auto open = text.find('(');
    if (open == std::string::npos || text.back() != ')') {
        return std::nullopt;
    }
    const auto name = trim(text.substr(0, open));
    if (name.empty() || !std::all_of(name.begin(), name.end(), identifierChar)) {
        return std::nullopt;
    }

    Directive directive;
    directive.name = lower(name);
    const auto inner = text.substr(open + 1, text.size() - open - 2);
    std::string argument;
    for (const char c : inner) {
        if (c == ',') {
            directive.args.push_back(trim(argument));
            argument.clear();
            continue;
        }
        argument.push_back(c);
    }
    if (!trim(argument).empty() || !directive.args.empty()) {
        directive.args.push_back(trim(argument));
    }
    return directive;
}

// `name:` on a line of its own. A trailing instruction on the same line is not
// accepted, deliberately: `label: mov eax,1` reads as two things and would have
// to be split anyway, and refusing it is clearer than silently splitting it.
std::optional<std::string> parseLabelDefinition(const std::string& text) {
    if (text.size() < 2 || text.back() != ':') {
        return std::nullopt;
    }
    const auto name = trim(text.substr(0, text.size() - 1));
    if (name.empty() || !std::all_of(name.begin(), name.end(), identifierChar) ||
        std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
        return std::nullopt;
    }
    return name;
}

// A line ending in ':' whose name is not a plain identifier: `0x7FF612340000:`
// or `game.exe+8A3F1:`. Such a line can only be an origin -- there is nothing to
// declare -- and it is resolved through the symbol table like every other
// address box in the tool.
//
// This is not an exotic case. `define(INJECT, 7FF612340000)` followed by
// `INJECT:` is the ordinary way a fixed-address injection is written, and the
// define has already turned the label into an address by the time this sees it.
std::optional<std::string> parseOriginExpression(const std::string& text) {
    if (text.size() < 2 || text.back() != ':') {
        return std::nullopt;
    }
    const auto name = trim(text.substr(0, text.size() - 1));
    // No spaces: `mov eax, 1:` is a mistake, not an origin.
    if (name.empty() || name.find_first_of(" \t") != std::string::npos) {
        return std::nullopt;
    }
    return name;
}

std::size_t dataWidth(const std::string& mnemonic) {
    if (mnemonic == "db") return 1;
    if (mnemonic == "dw") return 2;
    if (mnemonic == "dd") return 4;
    if (mnemonic == "dq") return 8;
    return 0;
}

// db/dw/dd/dq. Values are hexadecimal, comma- or space-separated, and written
// little-endian in the directive's width -- the same convention as everywhere
// else that shows bytes in this tool.
std::optional<std::vector<std::uint8_t>> parseData(const std::string& text) {
    std::istringstream in(text);
    std::string mnemonic;
    in >> mnemonic;
    const auto width = dataWidth(lower(mnemonic));
    if (width == 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    std::string rest;
    std::getline(in, rest);
    std::string token;
    const auto flush = [&]() -> bool {
        if (token.empty()) {
            return true;
        }
        const auto value = domain::parseAddress(token);
        if (!value) {
            return false;
        }
        auto number = static_cast<std::uint64_t>(*value);
        for (std::size_t i = 0; i < width; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(number & 0xFF));
            number >>= 8;
        }
        token.clear();
        return true;
    };
    for (const char c : rest) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!flush()) {
                return std::nullopt;
            }
            continue;
        }
        token.push_back(c);
    }
    if (!flush() || bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

// Whole-identifier replacement. Only names the caller already knows about are
// touched, so `mov [eax+4], ecx` passes through untouched however many of its
// letters happen to spell something.
std::string substituteText(const std::string& text, const DefineMap& defines) {
    if (defines.empty()) {
        return text;
    }
    std::string out;
    std::string identifier;
    const auto flush = [&]() {
        if (identifier.empty()) {
            return;
        }
        const auto found = defines.find(identifier);
        out += found != defines.end() ? found->second : identifier;
        identifier.clear();
    };
    for (const char c : text) {
        if (identifierChar(c)) {
            identifier.push_back(c);
        } else {
            flush();
            out.push_back(c);
        }
    }
    flush();
    return out;
}

// Symbols reach Keystone as hexadecimal literals; it has no idea what a label
// of ours is. Built once per layout pass rather than per line.
DefineMap asSubstitutions(const SymbolMap& symbols) {
    DefineMap out;
    for (const auto& [name, address] : symbols) {
        out.emplace(name, domain::toHex(address));
    }
    return out;
}

} // namespace

AutoAssembler::AutoAssembler(domain::TargetSession& session, const engine_asm::Assembler& assembler,
                             engine_patch::PatchRegistry& patches, engine_symbols::SymbolTable& symbols,
                             engine_inject::Injector& injector)
    : session_(session), assembler_(assembler), patches_(patches), symbols_(symbols), injector_(injector) {}

infra::Result<CompileResult> AutoAssembler::compile(const std::string& source, bool enableSection, bool execute,
                                                    RunState* state) {
    CompileResult result;
    const auto lines = sectionLines(source, enableSection);
    if (lines.empty()) {
        return Result<CompileResult>::ok(std::move(result));
    }
    if (!session_.attached()) {
        return Result<CompileResult>::fail("Attach to a process before running a script.");
    }

    const auto fail = [](const Line& line, const std::string& message) {
        return Result<CompileResult>::fail("Line " + std::to_string(line.number) + ": " + message);
    };

    SymbolMap symbols;
    DefineMap defines;
    // The [DISABLE] section names symbols the [ENABLE] run computed, so it
    // starts from that run's map rather than from nothing.
    if (state != nullptr) {
        for (const auto& [name, address] : state->symbols) {
            symbols[name] = address;
        }
    }

    // Ranges of memory this run allocated. A write landing inside one is a
    // write into fresh pages, which needs no undo record.
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> allocatedRanges;
    const auto insideAllocation = [&allocatedRanges](std::uintptr_t address) {
        return std::any_of(allocatedRanges.begin(), allocatedRanges.end(),
                           [address](const auto& range) {
                               return address >= range.first && address < range.second;
                           });
    };

    // ---------------------------------------------------------------------
    // Pass one: the directives, in source order, because a later one routinely
    // depends on an earlier one -- alloc(newmem, 0x100, INJECT) cannot place
    // itself until aobscanmodule has said where INJECT is.
    // ---------------------------------------------------------------------
    std::vector<std::string> declaredLabels;
    // Fake addresses for a check run, so the layout still reasons about real
    // distances without allocating anything in the target.
    std::uintptr_t pretendArena = 0;

    for (const auto& line : lines) {
        const auto expanded = substituteText(line.text, defines);
        const auto directive = parseDirective(expanded);
        if (!directive) {
            if (auto label = parseLabelDefinition(expanded)) {
                declaredLabels.push_back(*label);
            }
            continue;
        }
        const auto& name = directive->name;
        const auto& args = directive->args;

        if (name == "define") {
            if (args.size() != 2 || args[0].empty()) {
                return fail(line, "define takes a name and the text to substitute for it.");
            }
            defines[args[0]] = args[1];
        } else if (name == "label") {
            if (args.size() != 1 || args[0].empty()) {
                return fail(line, "label takes one name.");
            }
            declaredLabels.push_back(args[0]);
        } else if (name == "alloc") {
            if (args.size() < 2 || args.size() > 3) {
                return fail(line, "alloc takes a name, a size, and optionally something to stay near.");
            }
            const auto size = domain::parseAddress(args[1]);
            if (!size || *size == 0) {
                return fail(line, "\"" + args[1] + "\" is not a size. Sizes are hexadecimal, like 0x100.");
            }
            std::uintptr_t hint = 0;
            if (args.size() == 3) {
                if (const auto found = symbols.find(args[2]); found != symbols.end()) {
                    hint = found->second;
                } else if (auto resolved = symbols_.resolve(session_, args[2])) {
                    // Not only a name this script has bound. A fixed-address
                    // injection writes define(INJECT, 7FF7194C3B38) and then
                    // alloc(cave, 0x100, INJECT), and by the time this sees it
                    // the define has already turned it into the address.
                    hint = resolved.value();
                } else {
                    return fail(line, "\"" + args[2] + "\" is not known yet, so the allocation cannot be "
                                                       "placed near it. Scan for it first.");
                }
            }

            std::uintptr_t address = 0;
            if (execute) {
                auto allocated = hint != 0
                                     ? injector_.allocateNear(*size, PAGE_EXECUTE_READWRITE, hint)
                                     : injector_.allocate(*size, PAGE_EXECUTE_READWRITE);
                if (!allocated) {
                    return fail(line, "alloc(" + args[0] + ") failed: " + allocated.error());
                }
                address = allocated.value();
            } else {
                // Somewhere plausible and within jump range of the hint, so a
                // check run reports the same encodings a real run would use.
                if (pretendArena == 0) {
                    pretendArena = hint != 0 ? hint + 0x10000 : 0x10000000;
                }
                address = pretendArena;
                pretendArena += *size + 0x1000;
            }

            symbols[args[0]] = address;
            declaredLabels.push_back(args[0]);
            allocatedRanges.emplace_back(address, address + *size);
            result.allocations.push_back({args[0], address, *size});
            result.notes.push_back("alloc " + args[0] + " = " + domain::toHex(address) + " (" +
                                   std::to_string(*size) + " bytes)" +
                                   (hint != 0 ? ", within reach of " + domain::toHex(hint) : "") +
                                   (execute ? "" : " [not actually allocated: this was a check]"));
        } else if (name == "aobscanmodule") {
            if (args.size() != 3) {
                return fail(line, "aobscanmodule takes a name, a module, and a byte pattern.");
            }
            const auto pattern = domain::parseHexPattern(args[2]);
            if (!pattern) {
                return fail(line, "\"" + args[2] + "\" is not a byte pattern. Use hex bytes and ?? for "
                                                   "wildcards, like 89 46 04 8B ?? 08.");
            }
            const auto base = engine_symbols::ExportResolver::moduleBase(session_, domain::widen(args[1]));
            if (base == 0) {
                return fail(line, args[1] + " is not loaded in the target.");
            }
            std::size_t moduleSize = 0;
            for (const auto& module : session_.modules()) {
                if (module.base == base) {
                    moduleSize = module.size;
                    break;
                }
            }
            const auto matches = engine_scan::findPattern(session_, base, moduleSize, *pattern);
            if (matches.empty()) {
                return fail(line, "The pattern was not found in " + args[1] +
                                      ". If the target has been updated since this script was written, the "
                                      "code it names has probably moved or changed.");
            }
            if (matches.size() > 1) {
                // Taking the first would produce a script that works today and
                // patches something else after the next update. A pattern that
                // matches twice is a pattern that is not specific enough.
                return fail(line, "The pattern matches " + std::to_string(matches.size()) + " places in " +
                                      args[1] + " (" + domain::toHex(matches[0]) + ", " +
                                      domain::toHex(matches[1]) +
                                      ", ...). Lengthen it until it matches exactly one.");
            }
            symbols[args[0]] = matches[0];
            declaredLabels.push_back(args[0]);
            result.notes.push_back("aobscanmodule " + args[0] + " = " + domain::toHex(matches[0]) + " (" +
                                   args[1] + "+" + domain::toHex(matches[0] - base) + ")");
        } else if (name == "assert") {
            if (args.size() != 2) {
                return fail(line, "assert takes an address and the bytes that should be there.");
            }
            const auto expected = domain::parseHexPattern(args[1]);
            if (!expected) {
                return fail(line, "\"" + args[1] + "\" is not a byte pattern.");
            }
            std::uintptr_t address = 0;
            if (const auto found = symbols.find(args[0]); found != symbols.end()) {
                address = found->second;
            } else if (auto resolved = symbols_.resolve(session_, args[0])) {
                address = resolved.value();
            } else {
                return fail(line, "assert could not work out where \"" + args[0] + "\" is: " + resolved.error());
            }
            auto actual = session_.readBytes(address, expected->bytes.size());
            if (!actual || actual.value().size() != expected->bytes.size()) {
                return fail(line, "assert could not read " + std::to_string(expected->bytes.size()) +
                                      " byte(s) at " + domain::toHex(address) + ".");
            }
            for (std::size_t i = 0; i < expected->bytes.size(); ++i) {
                if (expected->mask[i] != 0 && actual.value()[i] != expected->bytes[i]) {
                    return fail(line, "assert failed at " + domain::toHex(address) + ": expected " +
                                          domain::bytesToHex(expected->bytes) + " but found " +
                                          domain::bytesToHex(actual.value()) +
                                          ". Nothing has been written. This is the check doing its job -- the "
                                          "code this script was written against is not what is there now.");
                }
            }
            result.notes.push_back("assert " + args[0] + " ok");
        } else if (name == "registersymbol") {
            if (args.size() != 1 || args[0].empty()) {
                return fail(line, "registersymbol takes one name.");
            }
            result.registerSymbols.emplace_back(args[0], 0); // value filled in after layout
        } else if (name == "unregistersymbol") {
            if (args.size() != 1 || args[0].empty()) {
                return fail(line, "unregistersymbol takes one name.");
            }
            result.unregisterSymbols.push_back(args[0]);
        } else if (name == "dealloc") {
            if (args.size() != 1 || args[0].empty()) {
                return fail(line, "dealloc takes one name.");
            }
            result.deallocations.push_back(args[0]);
        } else {
            return fail(line, "\"" + directive->name + "\" is not a directive this assembler knows.");
        }
    }

    // ---------------------------------------------------------------------
    // Pass two: layout, iterated to a fixed point.
    //
    // An instruction's length can depend on a label's value -- a jump to
    // somewhere close encodes in two bytes and somewhere far in five -- and a
    // label's value depends on the lengths of everything before it. So the
    // layout is computed repeatedly until it stops moving. It converges in one
    // or two passes for anything a person would write; the cap is a backstop
    // against a script that oscillates rather than settles.
    // ---------------------------------------------------------------------
    const auto bitness = session_.bitness();

    // What a label is worth before the pass that defines it has run. It has to
    // be far enough away that the first pass picks the longest encodings and the
    // layout shrinks towards the answer rather than growing past it -- but still
    // close enough to *have* an encoding. A fixed low constant fails that second
    // half on x64, where a module sits at something like 0x7FF7194C0000 and a
    // rel32 jump to 0x7FFF0000 cannot be encoded at all: the first pass then
    // reports a Keystone error for a script that is perfectly fine.
    //
    // Half a gigabyte above the lowest address the script already knows. Every
    // origin in a script is within 2 GB of every other -- that is what alloc's
    // `near` argument guarantees -- so this is in rel32 range of all of them.
    std::uintptr_t lowestKnown = 0;
    for (const auto& [name, address] : symbols) {
        if (lowestKnown == 0 || address < lowestKnown) {
            lowestKnown = address;
        }
    }
    const std::uintptr_t placeholder = lowestKnown != 0 ? lowestKnown + 0x20000000 : 0x20000000;

    SymbolMap layout = symbols;
    for (const auto& label : declaredLabels) {
        layout.emplace(label, placeholder);
    }

    std::vector<Block> blocks;
    bool settled = false;
    for (int pass = 0; pass < maxLayoutPasses && !settled; ++pass) {
        SymbolMap next = symbols;
        const auto substitutions = asSubstitutions(layout);
        blocks.clear();
        std::uintptr_t writeAddress = 0;
        bool addressKnown = false;

        for (const auto& line : lines) {
            const auto expanded = substituteText(line.text, defines);
            if (parseDirective(expanded)) {
                continue;
            }

            if (auto label = parseLabelDefinition(expanded)) {
                if (const auto anchor = symbols.find(*label); anchor != symbols.end()) {
                    // A name that already has a value -- an allocation, or the
                    // result of a scan -- is an *origin*: what follows is
                    // assembled at that address.
                    writeAddress = anchor->second;
                    addressKnown = true;
                    blocks.push_back({writeAddress, {}, insideAllocation(writeAddress)});
                } else {
                    if (!addressKnown) {
                        return fail(line, "\"" + *label +
                                              "\" has nothing to be relative to yet. Start the section at an "
                                              "allocation or a scanned address.");
                    }
                    next[*label] = writeAddress;
                }
                continue;
            }

            if (auto origin = parseOriginExpression(expanded)) {
                auto resolved = symbols_.resolve(session_, *origin);
                if (!resolved) {
                    return fail(line, "\"" + *origin +
                                          "\" is not somewhere this script can assemble at: " + resolved.error());
                }
                writeAddress = resolved.value();
                addressKnown = true;
                blocks.push_back({writeAddress, {}, insideAllocation(writeAddress)});
                continue;
            }

            if (!addressKnown) {
                return fail(line, "There is no address to assemble at yet. A section begins by naming one, "
                                  "usually with aobscanmodule or alloc.");
            }

            std::vector<std::uint8_t> bytes;
            if (auto data = parseData(expanded)) {
                bytes = std::move(*data);
            } else if (dataWidth(lower(expanded.substr(0, expanded.find_first_of(" \t")))) != 0) {
                return fail(line, "\"" + expanded + "\" is not readable as data. Values are hexadecimal.");
            } else {
                const auto assembly = substituteText(expanded, substitutions);
                auto assembled = assembler_.assemble(assembly, writeAddress, bitness);
                if (!assembled) {
                    return fail(line, assembled.error());
                }
                bytes = std::move(assembled.value());
            }

            if (blocks.empty() || blocks.back().address + blocks.back().bytes.size() != writeAddress) {
                blocks.push_back({writeAddress, {}, insideAllocation(writeAddress)});
            }
            blocks.back().bytes.insert(blocks.back().bytes.end(), bytes.begin(), bytes.end());
            writeAddress += bytes.size();
        }

        // Only the labels this pass actually computed are compared. The
        // placeholder entries for names that are never defined would otherwise
        // never match, and they are caught separately below.
        settled = true;
        for (const auto& [name, address] : next) {
            const auto previous = layout.find(name);
            if (previous == layout.end() || previous->second != address) {
                settled = false;
            }
            layout[name] = address;
        }
    }

    if (!settled) {
        return Result<CompileResult>::fail(
            "The layout did not settle after " + std::to_string(maxLayoutPasses) +
            " passes: an instruction's length keeps changing the label it depends on. Force a long encoding "
            "(for example `jmp near`) to break the cycle.");
    }

    // Any label still sitting on its placeholder was declared and never defined.
    for (const auto& label : declaredLabels) {
        if (layout[label] == placeholder && symbols.find(label) == symbols.end()) {
            return Result<CompileResult>::fail("\"" + label +
                                               "\" was declared with label() but never defined. Add a line "
                                               "reading \"" + label + ":\" where it belongs.");
        }
    }

    for (auto& [name, address] : result.registerSymbols) {
        const auto found = layout.find(name);
        if (found == layout.end()) {
            return Result<CompileResult>::fail("registersymbol(" + name + ") names something the script never "
                                                                          "defines.");
        }
        address = found->second;
    }

    // Empty blocks come from an origin label with nothing after it.
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                [](const Block& block) { return block.bytes.empty(); }),
                 blocks.end());
    result.blocks = std::move(blocks);

    if (state != nullptr) {
        for (const auto& [name, address] : layout) {
            state->symbols[name] = address;
        }
    }
    return Result<CompileResult>::ok(std::move(result));
}

infra::Result<CompileResult> AutoAssembler::check(const std::string& source, bool enableSection) {
    return compile(source, enableSection, false, nullptr);
}

infra::Result<void> AutoAssembler::runEnable(Script& script, RunState& state) {
    auto compiled = compile(script.source, true, true, &state);
    if (!compiled) {
        return infra::Result<void>::fail(compiled.error());
    }

    // Everything this run created, so a failure part-way through can be undone
    // rather than left half-applied.
    const auto rollback = [&]() {
        for (auto id = state.patchIds.rbegin(); id != state.patchIds.rend(); ++id) {
            if (auto removed = patches_.remove(*id); !removed) {
                infra::Logger::instance().error("Rolling back a failed script: " + removed.error());
            }
        }
        state.patchIds.clear();
        for (const auto& allocation : compiled.value().allocations) {
            if (auto freed = injector_.free(allocation.address); !freed) {
                infra::Logger::instance().error("Rolling back a failed script: could not free " +
                                                domain::toHex(allocation.address) + ": " + freed.error());
            }
        }
    };

    for (const auto& block : compiled.value().blocks) {
        if (block.intoAllocation) {
            // Fresh pages nobody has ever seen. Recording an "original" of
            // uninitialised memory would be noise in the Patches panel, and
            // freeing the allocation is the undo.
            if (auto written = session_.writeBytes(block.address, block.bytes); !written) {
                rollback();
                return infra::Result<void>::fail("Could not write " + std::to_string(block.bytes.size()) +
                                                 " byte(s) into the allocation at " +
                                                 domain::toHex(block.address) + ": " + written.error());
            }
            continue;
        }

        auto applied = patches_.apply(block.address, block.bytes, "script: " + script.name);
        if (!applied) {
            rollback();
            return infra::Result<void>::fail("Could not patch " + domain::toHex(block.address) + ": " +
                                             applied.error());
        }
        state.patchIds.push_back(applied.value());
    }

    for (const auto& [name, address] : compiled.value().registerSymbols) {
        if (auto defined = symbols_.define(name, address); !defined) {
            infra::Logger::instance().warn("registersymbol(" + name + ") was ignored: " + defined.error());
        } else {
            state.registeredSymbols.push_back(name);
        }
    }

    state.allocations = compiled.value().allocations;
    state.notes = compiled.value().notes;
    return infra::Result<void>::ok();
}

infra::Result<void> AutoAssembler::runDisable(Script& script, RunState& state) {
    std::vector<std::string> problems;

    // The registry first, and in reverse. Its records are the authoritative
    // undo, and the jump into a cave has to be gone before the cave is freed --
    // otherwise a thread already on its way there lands in unmapped memory.
    for (auto id = state.patchIds.rbegin(); id != state.patchIds.rend(); ++id) {
        if (auto removed = patches_.remove(*id); !removed) {
            problems.push_back(removed.error());
        }
    }
    state.patchIds.clear();

    // Then whatever the [DISABLE] section itself writes. It runs second so an
    // author who wants different bytes than the recorded original gets them,
    // rather than having the registry overwrite their intent.
    auto compiled = compile(script.source, false, true, &state);
    if (!compiled) {
        problems.push_back(compiled.error());
    } else {
        for (const auto& block : compiled.value().blocks) {
            if (auto written = session_.writeBytes(block.address, block.bytes); !written) {
                problems.push_back("Could not write the [DISABLE] bytes at " + domain::toHex(block.address) +
                                   ": " + written.error());
            }
        }
        for (const auto& name : compiled.value().unregisterSymbols) {
            symbols_.undefine(name);
        }
        // dealloc names are resolved against this script's own allocations;
        // freeing an address the script did not allocate would be freeing
        // someone else's memory.
        for (const auto& name : compiled.value().deallocations) {
            const auto allocation = std::find_if(state.allocations.begin(), state.allocations.end(),
                                                 [&name](const Allocation& a) { return a.name == name; });
            if (allocation == state.allocations.end()) {
                problems.push_back("dealloc(" + name + ") names something this script did not allocate.");
                continue;
            }
            if (auto freed = injector_.free(allocation->address); !freed) {
                problems.push_back("Could not free " + domain::toHex(allocation->address) + ": " +
                                   freed.error());
            }
        }
    }

    for (const auto& name : state.registeredSymbols) {
        symbols_.undefine(name);
    }
    state.registeredSymbols.clear();
    state.allocations.clear();
    state.symbols.clear();
    state.notes = compiled ? compiled.value().notes : std::vector<std::string>{};

    if (!problems.empty()) {
        std::string message = "The script was disabled, but " + std::to_string(problems.size()) +
                              " step(s) did not complete:";
        for (const auto& problem : problems) {
            message += "\n  - " + problem;
        }
        return infra::Result<void>::fail(message);
    }
    return infra::Result<void>::ok();
}

std::uint64_t AutoAssembler::add(std::string name, std::string source) {
    std::scoped_lock lock(mutex_);
    Script script;
    script.id = nextId_++;
    script.name = name.empty() ? "Script " + std::to_string(script.id) : std::move(name);
    script.source = std::move(source);
    scripts_.push_back(script);
    return script.id;
}

infra::Result<void> AutoAssembler::update(std::uint64_t id, std::string name, std::string source) {
    std::scoped_lock lock(mutex_);
    const auto script = std::find_if(scripts_.begin(), scripts_.end(),
                                     [id](const Script& s) { return s.id == id; });
    if (script == scripts_.end()) {
        return infra::Result<void>::fail("That script is no longer in the list.");
    }
    if (script->enabled) {
        // The [DISABLE] section that would put the target back is the one in
        // the source being replaced. Editing it while it is running is how a
        // patch ends up with no way back.
        return infra::Result<void>::fail("Turn the script off before editing it. The section that undoes it is "
                                         "part of the text you are about to replace.");
    }
    if (!name.empty()) {
        script->name = std::move(name);
    }
    script->source = std::move(source);
    return infra::Result<void>::ok();
}

infra::Result<void> AutoAssembler::setEnabled(std::uint64_t id, bool enabled) {
    Script copy;
    {
        std::scoped_lock lock(mutex_);
        const auto script = std::find_if(scripts_.begin(), scripts_.end(),
                                         [id](const Script& s) { return s.id == id; });
        if (script == scripts_.end()) {
            return infra::Result<void>::fail("That script is no longer in the list.");
        }
        if (script->enabled == enabled) {
            return infra::Result<void>::ok();
        }
        copy = *script;
    }

    // Run outside the lock: enabling reads and writes the target, and holding
    // the list lock across that would stall the UI thread polling scripts().
    RunState state;
    {
        std::scoped_lock lock(mutex_);
        if (const auto found = state_.find(id); found != state_.end()) {
            state = found->second;
        }
    }

    auto result = enabled ? runEnable(copy, state) : runDisable(copy, state);

    std::scoped_lock lock(mutex_);
    const auto script = std::find_if(scripts_.begin(), scripts_.end(),
                                     [id](const Script& s) { return s.id == id; });
    if (script != scripts_.end()) {
        // A disable that hit problems still leaves the script off: its patches
        // are gone, and offering to "disable again" would have nothing to undo.
        script->enabled = enabled && result.has_value();
    }
    state_[id] = std::move(state);
    return result;
}

infra::Result<void> AutoAssembler::remove(std::uint64_t id) {
    if (const auto script = find(id); script && script->enabled) {
        if (auto disabled = setEnabled(id, false); !disabled) {
            // Deliberately kept. A script whose patches could not be restored
            // is exactly the one the user still needs in the list.
            return disabled;
        }
    }
    std::scoped_lock lock(mutex_);
    const auto removed = std::remove_if(scripts_.begin(), scripts_.end(),
                                        [id](const Script& s) { return s.id == id; });
    if (removed == scripts_.end()) {
        return infra::Result<void>::fail("That script is no longer in the list.");
    }
    scripts_.erase(removed, scripts_.end());
    state_.erase(id);
    return infra::Result<void>::ok();
}

infra::Result<void> AutoAssembler::disableAll() {
    std::vector<std::uint64_t> enabled;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& script : scripts_) {
            if (script.enabled) {
                enabled.push_back(script.id);
            }
        }
    }

    std::size_t failures{};
    for (const auto id : enabled) {
        if (auto disabled = setEnabled(id, false); !disabled) {
            infra::Logger::instance().error("Could not disable a script: " + disabled.error());
            ++failures;
        }
    }
    if (failures != 0) {
        return infra::Result<void>::fail(std::to_string(failures) + " of " + std::to_string(enabled.size()) +
                                         " script(s) did not disable cleanly; see the log.");
    }
    return infra::Result<void>::ok();
}

void AutoAssembler::forgetAll() {
    std::scoped_lock lock(mutex_);
    scripts_.clear();
    state_.clear();
}

std::vector<Script> AutoAssembler::scripts() const {
    std::scoped_lock lock(mutex_);
    return scripts_;
}

std::optional<Script> AutoAssembler::find(std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    const auto script = std::find_if(scripts_.begin(), scripts_.end(),
                                     [id](const Script& s) { return s.id == id; });
    if (script == scripts_.end()) {
        return std::nullopt;
    }
    return *script;
}

std::vector<std::string> AutoAssembler::lastNotes(std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    const auto found = state_.find(id);
    return found == state_.end() ? std::vector<std::string>{} : found->second.notes;
}

std::string AutoAssembler::makeTemplate(Template shape, std::uintptr_t address,
                                        const std::vector<std::uint8_t>& bytes,
                                        const std::string& moduleName) const {
    const auto pattern = bytes.empty() ? std::string("?? ?? ?? ?? ??") : domain::bytesToHex(bytes);
    const auto module = moduleName.empty() ? std::string("game.exe") : moduleName;
    const auto size = std::to_string(std::max<std::size_t>(bytes.size(), 5));

    std::ostringstream out;
    switch (shape) {
    case Template::AobInjection:
        out << "// AOB injection.\n"
            << "//\n"
            << "// The address is found by the bytes around it rather than written down,\n"
            << "// so the script still works after the target is rebuilt and everything\n"
            << "// moves -- as long as the code itself has not changed.\n"
            << "\n[ENABLE]\n"
            << "aobscanmodule(INJECT, " << module << ", " << pattern << ")\n"
            << "alloc(newmem, 0x100, INJECT)\n"
            << "label(code)\n"
            << "label(return)\n"
            << "registersymbol(INJECT)\n"
            << "\n"
            << "newmem:\n"
            << "code:\n"
            << "  // Replacement for the instruction(s) at INJECT goes here.\n"
            << "  jmp return\n"
            << "\n"
            << "INJECT:\n"
            << "  jmp newmem\n"
            << "return:\n"
            << "\n[DISABLE]\n"
            << "INJECT:\n"
            << "  db " << pattern << "\n"
            << "\n"
            << "unregistersymbol(INJECT)\n"
            << "dealloc(newmem)\n";
        break;
    case Template::CodeCave:
        out << "// Code cave.\n"
            << "//\n"
            << "// A fixed address rather than a pattern: quicker to write, and good for\n"
            << "// as long as the target is not rebuilt. The assert is what stops it\n"
            << "// writing over the wrong instruction when it is.\n"
            << "\n[ENABLE]\n"
            << "define(INJECT, " << domain::toHex(address) << ")\n"
            << "assert(INJECT, " << pattern << ")\n"
            << "alloc(cave, 0x100, INJECT)\n"
            << "label(return)\n"
            << "\n"
            << "cave:\n"
            << "  // Your code here.\n"
            << "  jmp return\n"
            << "\n"
            << "INJECT:\n"
            << "  jmp cave\n"
            << "return:\n"
            << "\n[DISABLE]\n"
            << "define(INJECT, " << domain::toHex(address) << ")\n"
            << "INJECT:\n"
            << "  db " << pattern << "\n"
            << "\n"
            << "dealloc(cave)\n";
        break;
    case Template::FullInjection:
        out << "// Full injection: a cave that keeps the original instruction, runs\n"
            << "// alongside it, and jumps back.\n"
            << "//\n"
            << "// The original instruction is executed in the cave rather than thrown\n"
            << "// away. One instruction usually does more than the one thing you noticed,\n"
            << "// and dropping it is the most common way an injection breaks a game.\n"
            << "\n[ENABLE]\n"
            << "aobscanmodule(INJECT, " << module << ", " << pattern << ")\n"
            << "alloc(newmem, 0x200, INJECT)\n"
            << "label(originalcode)\n"
            << "label(return)\n"
            << "registersymbol(INJECT)\n"
            << "\n"
            << "newmem:\n"
            << "  // Your code here. Registers are exactly as the game left them,\n"
            << "  // so save anything you clobber.\n"
            << "\n"
            << "originalcode:\n"
            << "  db " << pattern << "  // the " << size << " byte(s) that were at INJECT\n"
            << "  jmp return\n"
            << "\n"
            << "INJECT:\n"
            << "  jmp newmem\n"
            << "return:\n"
            << "\n[DISABLE]\n"
            << "INJECT:\n"
            << "  db " << pattern << "\n"
            << "\n"
            << "unregistersymbol(INJECT)\n"
            << "dealloc(newmem)\n";
        break;
    }
    return out.str();
}

} // namespace ire::engine_aa
