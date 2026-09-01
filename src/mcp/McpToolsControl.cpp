// The tools that change the target rather than describe it: code, breakpoints,
// patches, the speed hack, and the two that drive the window itself.
//
// Every tool in this file can destabilise or crash the process it is pointed at.
// That is the nature of running code inside somebody else's program, and it is
// stated here rather than assumed because the caller is not necessarily a person
// who already knows it.

#include "mcp/McpInternal.h"
#include "mcp/McpTools.h"

#include "services/UiCommands.h"

#include <algorithm>

namespace ire::mcp {

namespace {

template <typename T>
using Result = infra::Result<T>;

Result<Json> notAttached() {
    return Result<Json>::fail("No target process is attached. Call attach with a pid first.");
}

// Tools that drive the window go through services::uiCommands(), which is null
// in a build with no frontend -- the tests, anything embedding the engines
// alone. They say so rather than failing oddly.
services::UiCommands* window() {
    return services::uiCommands();
}

Result<Json> noWindow() {
    return Result<Json>::fail("There is no window to drive.");
}

Json registersJson(const domain::RegisterContext& context) {
    if (!context.captured) {
        return Json(nullptr);
    }
    Json registers = Json::object();
    for (std::size_t i = 0; i < domain::registerCount(context.bitness); ++i) {
        registers[domain::registerName(context.bitness, i)] = domain::registerValue(context, i);
    }
    registers["eflags"] = context.eflags;
    registers["thread_id"] = context.threadId;
    return registers;
}

infra::Result<domain::BreakpointKind> parseBreakpointKind(const Json& args, const char* key) {
    using Kind = domain::BreakpointKind;
    const auto text = optionalString(args, key, "software");
    if (text == "software") { return infra::Result<Kind>::ok(Kind::Software); }
    if (text == "execute") { return infra::Result<Kind>::ok(Kind::HardwareExecute); }
    if (text == "write") { return infra::Result<Kind>::ok(Kind::HardwareWrite); }
    if (text == "readwrite") { return infra::Result<Kind>::ok(Kind::HardwareReadWrite); }
    return infra::Result<Kind>::fail("\"" + text +
                                     "\" is not a breakpoint kind. Use software, execute, write or readwrite.");
}

// Win32 PAGE_* constants, named rather than numbered. A caller passing 0x40
// would be passing a number whose meaning it has to look up, and getting it
// wrong allocates memory the target cannot execute.
infra::Result<DWORD> parseProtection(const Json& args, const char* key) {
    const auto text = optionalString(args, key, "rwx");
    if (text == "rwx") { return infra::Result<DWORD>::ok(PAGE_EXECUTE_READWRITE); }
    if (text == "rw") { return infra::Result<DWORD>::ok(PAGE_READWRITE); }
    if (text == "rx") { return infra::Result<DWORD>::ok(PAGE_EXECUTE_READ); }
    if (text == "r") { return infra::Result<DWORD>::ok(PAGE_READONLY); }
    return infra::Result<DWORD>::fail("\"" + text + "\" is not a protection. Use rwx, rw, rx or r.");
}

} // namespace

// ---------------------------------------------------------------------------
// Code: disassembly, assembly, injection, auto-assembler
// ---------------------------------------------------------------------------

void ToolRegistry::registerCode() {
    add({"disassemble",
         "Decode instructions at an address. Bytes that do not decode come back with valid false "
         "rather than being skipped, so the listing stays aligned.",
         objectSchema(Json{{"address", addressProp("Where to start.")},
                           {"count", prop("integer", "How many instructions. Defaults to 16, capped at 200.")}},
                      {"address"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             const auto count = std::min<std::size_t>(
                 static_cast<std::size_t>(optionalUint(args, "count", 16)), 200);
             const auto instructions =
                 services_.disassembler().disassemble(services_.session(), address.value(), count);
             Json list = Json::array();
             for (const auto& instruction : instructions) {
                 Json row = addressJson(instruction.address);
                 row["text"] = instruction.text;
                 row["bytes"] = domain::bytesToHex(instruction.bytes, false);
                 row["valid"] = instruction.valid;
                 if (instruction.branchTarget != 0) {
                     row["branch_target"] = static_cast<std::uint64_t>(instruction.branchTarget);
                     row["branch_target_hex"] = domain::toHex(instruction.branchTarget);
                 }
                 list.push_back(std::move(row));
             }
             return Result<Json>::ok(Json{{"instructions", std::move(list)}});
         },
         false});

    add({"assemble",
         "Assemble Intel-syntax x86 to bytes, as if placed at a given address. The address is not "
         "decoration: a relative jmp or call encodes a different displacement depending on where "
         "the code will actually live.",
         objectSchema(Json{{"source", prop("string", "One instruction per line.")},
                           {"address", addressProp("Where the code will live.")}},
                      {"source", "address"}),
         [this](const Json& args) {
             const auto source = requireString(args, "source");
             if (!source) {
                 return Result<Json>::fail(source.error());
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             // The target's width, because several instructions assemble in both
             // modes to different bytes: a 64-bit encoding written into a 32-bit
             // process is accepted by the assembler and crashes the target.
             const auto bitness = services_.session().bitness();
             auto bytes = services_.assembler().assemble(source.value(), address.value(), bitness);
             if (!bytes) {
                 return Result<Json>::fail(bytes.error(), bytes.code());
             }
             Json out = addressJson(address.value());
             out["hex"] = domain::bytesToHex(bytes.value(), false);
             out["length"] = bytes.value().size();
             out["bitness"] = domain::bitnessName(bitness);
             return Result<Json>::ok(std::move(out));
         },
         false});

    add({"alloc",
         "Allocate memory inside the target. The allocation is not freed when you disconnect; it "
         "lives until the target exits.",
         objectSchema(Json{{"size", prop("integer", "How many bytes.")},
                           {"protection", enumProp({"rwx", "rw", "rx", "r"},
                                                   "Page protection. Defaults to rwx.")},
                           {"near", addressProp("Optional. Allocate within 2GB of this, so a five-byte "
                                                "jmp from there can reach it.")}},
                      {"size"}),
         [this](const Json& args) {
             const auto size = requireUint(args, "size");
             if (!size) {
                 return Result<Json>::fail(size.error());
             }
             const auto protection = parseProtection(args, "protection");
             if (!protection) {
                 return Result<Json>::fail(protection.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             auto allocated = Result<std::uintptr_t>::fail("Nothing was allocated.");
             if (has(args, "near")) {
                 const auto hint = requireAddress(services_, args, "near");
                 if (!hint) {
                     return Result<Json>::fail(hint.error(), hint.code());
                 }
                 allocated = services_.injector().allocateNear(static_cast<std::size_t>(size.value()),
                                                               protection.value(), hint.value());
             } else {
                 allocated = services_.injector().allocate(static_cast<std::size_t>(size.value()),
                                                           protection.value());
             }
             if (!allocated) {
                 return Result<Json>::fail(allocated.error(), allocated.code());
             }
             Json out = addressJson(allocated.value());
             out["size"] = size.value();
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"free",
         "Release a block previously returned by alloc. Freeing memory the target is still using "
         "will crash it.",
         objectSchema(Json{{"address", addressProp("Base of the block.")}}, {"address"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             auto freed = services_.injector().free(address.value());
             if (!freed) {
                 return Result<Json>::fail(freed.error(), freed.code());
             }
             return Result<Json>::ok(Json{{"freed", static_cast<std::uint64_t>(address.value())}});
         },
         true});

    add({"create_thread",
         "Start a thread in the target and wait up to five seconds for it. A thread still running "
         "after that is reported as such rather than given a bogus exit code, and it keeps running.",
         objectSchema(Json{{"start", addressProp("Where the thread begins.")},
                           {"parameter", prop("integer", "Its single argument. Defaults to 0.")}},
                      {"start"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto start = requireAddress(services_, args, "start");
             if (!start) {
                 return Result<Json>::fail(start.error(), start.code());
             }
             const auto parameter = static_cast<std::uintptr_t>(optionalUint(args, "parameter", 0));
             auto exitCode = services_.injector().createThread(start.value(), parameter);
             if (!exitCode) {
                 return Result<Json>::fail(exitCode.error(), exitCode.code());
             }
             return Result<Json>::ok(Json{{"exit_code", exitCode.value()}});
         },
         true});

    add({"load_library",
         "Load a DLL into the target via a remote LoadLibraryW. The library stays loaded until the "
         "target exits.",
         objectSchema(Json{{"path", prop("string", "Full path to the DLL.")}}, {"path"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto path = requireString(args, "path");
             if (!path) {
                 return Result<Json>::fail(path.error());
             }
             auto loaded = services_.injector().loadLibrary(domain::widen(path.value()));
             if (!loaded) {
                 return Result<Json>::fail(loaded.error(), loaded.code());
             }
             services_.session().refresh();
             return Result<Json>::ok(Json{{"path", path.value()}, {"result", loaded.value()}});
         },
         true});

    add({"aa_check",
         "Compile an auto-assembler script without writing or allocating anything, and report what "
         "it would do. This is how a script is read critically before it runs -- scans and asserts "
         "do read the target, but nothing is changed.",
         objectSchema(Json{{"source", prop("string", "The script, with [ENABLE] and [DISABLE] sections.")},
                           {"enable_section", prop("boolean", "Check [ENABLE] rather than [DISABLE]. "
                                                              "Defaults to true.")}},
                      {"source"}),
         [this](const Json& args) {
             const auto source = requireString(args, "source");
             if (!source) {
                 return Result<Json>::fail(source.error());
             }
             auto compiled = services_.autoAssembler().check(source.value(),
                                                             optionalBool(args, "enable_section", true));
             if (!compiled) {
                 return Result<Json>::fail(compiled.error(), compiled.code());
             }
             Json blocks = Json::array();
             for (const auto& block : compiled.value().blocks) {
                 Json entry = addressJson(block.address);
                 entry["length"] = block.bytes.size();
                 entry["hex"] = domain::bytesToHex(block.bytes, false);
                 entry["into_allocation"] = block.intoAllocation;
                 blocks.push_back(std::move(entry));
             }
             Json allocations = Json::array();
             for (const auto& allocation : compiled.value().allocations) {
                 allocations.push_back(Json{{"name", allocation.name},
                                            {"address", static_cast<std::uint64_t>(allocation.address)},
                                            {"size", allocation.size}});
             }
             return Result<Json>::ok(Json{{"blocks", std::move(blocks)},
                                          {"allocations", std::move(allocations)},
                                          {"notes", compiled.value().notes}});
         },
         true});

    add({"aa_add",
         "Add an auto-assembler script. It arrives switched off; use aa_set_enabled to run its "
         "[ENABLE] section.",
         objectSchema(Json{{"name", prop("string", "What to call it.")},
                           {"source", prop("string", "The script.")}},
                      {"name", "source"}),
         [this](const Json& args) {
             const auto name = requireString(args, "name");
             if (!name) {
                 return Result<Json>::fail(name.error());
             }
             const auto source = requireString(args, "source");
             if (!source) {
                 return Result<Json>::fail(source.error());
             }
             const auto id = services_.autoAssembler().add(name.value(), source.value());
             return Result<Json>::ok(Json{{"id", id}, {"name", name.value()}});
         },
         true});

    add({"aa_set_enabled",
         "Run a script's [ENABLE] section, or its [DISABLE] section to put the target back.",
         objectSchema(Json{{"id", prop("integer", "Script id.")},
                           {"enabled", prop("boolean", "True to enable, false to disable.")}},
                      {"id", "enabled"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             const bool enabled = optionalBool(args, "enabled", true);
             auto applied = services_.autoAssembler().setEnabled(id.value(), enabled);
             if (!applied) {
                 return Result<Json>::fail(applied.error(), applied.code());
             }
             return Result<Json>::ok(Json{{"id", id.value()},
                                          {"enabled", enabled},
                                          // "It worked" is much less useful than "INJECT resolved to
                                          // ac_client.exe+0x8A3F1", which is what these say.
                                          {"notes", services_.autoAssembler().lastNotes(id.value())}});
         },
         true});

    add({"aa_disable_all",
         "Switch off every enabled script, continuing past failures so one unwritable page cannot "
         "strand the rest.",
         emptySchema(),
         [this](const Json&) {
             auto disabled = services_.autoAssembler().disableAll();
             if (!disabled) {
                 return Result<Json>::fail(disabled.error(), disabled.code());
             }
             return Result<Json>::ok(Json{{"disabled", true}});
         },
         true});

    add({"aa_scripts",
         "Every auto-assembler script, with whether it is currently on.",
         emptySchema(),
         [this](const Json&) {
             Json list = Json::array();
             for (const auto& script : services_.autoAssembler().scripts()) {
                 list.push_back(Json{{"id", script.id},
                                     {"name", script.name},
                                     {"enabled", script.enabled},
                                     {"source", script.source}});
             }
             return Result<Json>::ok(Json{{"scripts", std::move(list)}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Breakpoints and the access watch
// ---------------------------------------------------------------------------

void ToolRegistry::registerBreakpoints() {
    add({"debugger_attach",
         "Attach the debugger to the target. Needed before any breakpoint can fire.",
         emptySchema(),
         [this](const Json&) {
             auto attached = services_.breakpoints().attachDebugger();
             if (!attached) {
                 return Result<Json>::fail(attached.error(), attached.code());
             }
             return Result<Json>::ok(Json{{"attached", true}});
         },
         true});

    add({"debugger_detach",
         "Detach the debugger, leaving the target running.",
         emptySchema(),
         [this](const Json&) {
             services_.breakpoints().detachDebugger();
             return Result<Json>::ok(Json{{"attached", false}});
         },
         true});

    add({"breakpoint_add",
         "Set a breakpoint. A software breakpoint replaces an instruction byte with int3 and there "
         "can be any number of them. A hardware one uses a debug register instead -- nothing in the "
         "target is modified and it can watch data rather than execution, but there are exactly "
         "four.",
         objectSchema(Json{{"address", addressProp("Where to break.")},
                           {"label", prop("string", "What to call it.")},
                           {"kind", enumProp({"software", "execute", "write", "readwrite"},
                                             "Defaults to software.")},
                           {"length", prop("integer", "Bytes a data breakpoint watches: 1, 2, 4 or 8. "
                                                      "The address must be aligned to that width.")}},
                      {"address"}),
         [this](const Json& args) {
             const auto kind = parseBreakpointKind(args, "kind");
             if (!kind) {
                 return Result<Json>::fail(kind.error());
             }
             const auto length = static_cast<std::uint8_t>(optionalUint(args, "length", 1));
             if (!domain::isValidWatchLength(length)) {
                 return Result<Json>::fail("length must be 1, 2, 4 or 8.");
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             auto added = services_.breakpoints().addBreakpoint(
                 address.value(), optionalString(args, "label", "MCP breakpoint"), kind.value(), length);
             if (!added) {
                 return Result<Json>::fail(added.error(), added.code());
             }
             Json out = addressJson(address.value());
             out["kind"] = domain::breakpointKindName(kind.value());
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"breakpoint_remove",
         "Remove a breakpoint, putting back whatever it changed.",
         objectSchema(Json{{"address", addressProp("The breakpoint's address.")}}, {"address"}),
         [this](const Json& args) {
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             auto removed = services_.breakpoints().removeBreakpoint(address.value());
             if (!removed) {
                 return Result<Json>::fail(removed.error(), removed.code());
             }
             return Result<Json>::ok(Json{{"removed", static_cast<std::uint64_t>(address.value())}});
         },
         true});

    add({"breakpoints_list",
         "Every breakpoint, its hit count, and the register state of the thread that most recently "
         "hit it.",
         emptySchema(),
         [this](const Json&) {
             Json list = Json::array();
             for (const auto& breakpoint : services_.breakpoints().breakpoints()) {
                 Json row = addressJson(breakpoint.address);
                 row["label"] = breakpoint.label;
                 row["kind"] = domain::breakpointKindName(breakpoint.kind);
                 row["enabled"] = breakpoint.enabled;
                 row["hit_count"] = breakpoint.hitCount;
                 row["registers"] = registersJson(breakpoint.lastHit);
                 list.push_back(std::move(row));
             }
             return Result<Json>::ok(Json{{"breakpoints", std::move(list)},
                                          {"debugger_attached", services_.breakpoints().debuggerAttached()}});
         },
         false});

    add({"breakpoint_events",
         "Breakpoint hit messages queued since the last call, removed as they are taken. Rate "
         "limited: a breakpoint in a hot loop fires far faster than anything can read, and the hit "
         "counts in breakpoints_list are the authoritative record.",
         emptySchema(),
         [this](const Json&) {
             return Result<Json>::ok(Json{{"events", services_.breakpoints().takeEvents()}});
         },
         true});

    add({"access_watch_start",
         "Find out what code reads or writes an address. Sets a hardware data breakpoint and "
         "aggregates every hit by the instruction responsible, which is what makes the answer "
         "readable rather than a flood.",
         objectSchema(Json{{"address", addressProp("The address to watch.")},
                           {"length", prop("integer", "Bytes to watch: 1, 2, 4 or 8. Defaults to 4.")},
                           {"writes_only", prop("boolean", "Only writes, rather than reads and writes. "
                                                           "Defaults to true.")}},
                      {"address"}),
         [this](const Json& args) {
             const auto length = static_cast<std::uint8_t>(optionalUint(args, "length", 4));
             if (!domain::isValidWatchLength(length)) {
                 return Result<Json>::fail("length must be 1, 2, 4 or 8.");
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             auto started = services_.startAccessWatch(address.value(), length,
                                                       optionalBool(args, "writes_only", true));
             if (!started) {
                 return Result<Json>::fail(started.error(), started.code());
             }
             Json out = addressJson(address.value());
             out["length"] = length;
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"access_watch_stop",
         "Stop the access watch and release its debug register.",
         emptySchema(),
         [this](const Json&) {
             services_.stopAccessWatch();
             return Result<Json>::ok(Json{{"active", false}});
         },
         true});

    add({"access_watch_sites",
         "The instructions seen touching the watched address, with what each register held. The "
         "interpretation is the point: \"watched address is RDI+0xF8\" says the structure's base is "
         "in RDI, which is the step from one address to every instance of the same object.",
         emptySchema(),
         [this](const Json&) {
             auto& watch = services_.accessWatch();
             Json list = Json::array();
             for (const auto& site : watch.sites()) {
                 Json row = addressJson(site.address);
                 row["text"] = site.text;
                 row["hit_count"] = site.hitCount;
                 // A data watchpoint traps *after* the access, so the reported
                 // instruction pointer names the one after the access. When the
                 // walk back failed, the address above is the trap address and
                 // saying so is better than a confident wrong answer.
                 row["instruction_resolved"] = site.instructionResolved;
                 row["trap_address_hex"] = domain::toHex(site.trapAddress);
                 Json meanings = Json::array();
                 for (const auto& meaning : watch.explain(site.lastContext)) {
                     if (meaning.interpretation.empty()) {
                         continue;
                     }
                     meanings.push_back(Json{{"register", meaning.name},
                                             {"value", meaning.value},
                                             {"means", meaning.interpretation}});
                 }
                 row["registers"] = std::move(meanings);
                 list.push_back(std::move(row));
             }
             return Result<Json>::ok(Json{{"active", watch.active()},
                                          {"watched_hex", domain::toHex(watch.watchedAddress())},
                                          {"total_hits", watch.totalHits()},
                                          {"truncated", watch.truncated()},
                                          {"sites", std::move(list)}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Patches
// ---------------------------------------------------------------------------

void ToolRegistry::registerPatches() {
    add({"patch_apply",
         "Overwrite code in the target, recording what was there so it can be put back. The patch "
         "is padded with nops to cover whole instructions, because a patch shorter than the code it "
         "replaces leaves a truncated instruction that crashes the target when execution reaches it.",
         objectSchema(Json{{"address", addressProp("Where to patch.")},
                           {"hex", prop("string", "Replacement bytes as hex.")},
                           {"description", prop("string", "What this patch is for.")},
                           {"pad", prop("boolean", "Pad to an instruction boundary. Defaults to true; "
                                                   "turn it off only if you have already done so.")}},
                      {"address", "hex"}),
         [this](const Json& args) {
             const auto hex = requireString(args, "hex");
             if (!hex) {
                 return Result<Json>::fail(hex.error());
             }
             auto bytes = domain::parseHexBytes(hex.value());
             if (bytes.empty()) {
                 return Result<Json>::fail("\"" + hex.value() + "\" contains no hex bytes.");
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             if (optionalBool(args, "pad", true)) {
                 bytes = engine_disasm::padToInstructionBoundary(services_.disassembler(),
                                                                 services_.session(), address.value(),
                                                                 std::move(bytes));
             }
             // The original instructions, so the patch list can say what was
             // replaced rather than only showing hex.
             std::string originalText;
             for (const auto& instruction : services_.disassembler().disassemble(
                      services_.session(), address.value(), 4)) {
                 if (instruction.address >= address.value() + bytes.size()) {
                     break;
                 }
                 if (!originalText.empty()) {
                     originalText += "; ";
                 }
                 originalText += instruction.text;
             }
             auto applied = services_.patches().apply(address.value(), bytes,
                                                      optionalString(args, "description", "MCP patch"),
                                                      originalText);
             if (!applied) {
                 return Result<Json>::fail(applied.error(), applied.code());
             }
             Json out = addressJson(address.value());
             out["id"] = applied.value();
             out["length"] = bytes.size();
             out["replaced"] = originalText;
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"patch_set_enabled",
         "Write a patch's replacement bytes, or put the originals back.",
         objectSchema(Json{{"id", prop("integer", "Patch id.")},
                           {"enabled", prop("boolean", "True to apply, false to restore.")}},
                      {"id", "enabled"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             const bool enabled = optionalBool(args, "enabled", true);
             auto applied = services_.patches().setEnabled(id.value(), enabled);
             if (!applied) {
                 return Result<Json>::fail(applied.error(), applied.code());
             }
             return Result<Json>::ok(Json{{"id", id.value()}, {"enabled", enabled}});
         },
         true});

    add({"patch_remove",
         "Restore the original bytes and forget the patch.",
         objectSchema(Json{{"id", prop("integer", "Patch id.")}}, {"id"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             auto removed = services_.patches().remove(id.value());
             if (!removed) {
                 return Result<Json>::fail(removed.error(), removed.code());
             }
             return Result<Json>::ok(Json{{"removed", id.value()}});
         },
         true});

    add({"patch_restore_all",
         "Disable every enabled patch, continuing past failures so one unwritable page cannot "
         "strand the rest.",
         emptySchema(),
         [this](const Json&) {
             auto restored = services_.patches().restoreAll();
             if (!restored) {
                 return Result<Json>::fail(restored.error(), restored.code());
             }
             return Result<Json>::ok(Json{{"restored", true}});
         },
         true});

    add({"patches_list",
         "Every recorded patch, and whether the bytes at its address still match what was written "
         "there -- drift means something else changed the code.",
         emptySchema(),
         [this](const Json&) {
             const auto attached = services_.session().attached();
             Json list = Json::array();
             for (const auto& patch : services_.patches().patches()) {
                 Json row = addressJson(patch.address);
                 row["id"] = patch.id;
                 row["description"] = patch.description;
                 row["enabled"] = patch.enabled;
                 row["length"] = patch.size();
                 row["original"] = domain::bytesToHex(patch.originalBytes, false);
                 row["patched"] = domain::bytesToHex(patch.patchedBytes, false);
                 row["original_text"] = patch.originalText;
                 if (attached) {
                     row["drifted"] = services_.patches().drifted(patch);
                 }
                 list.push_back(std::move(row));
             }
             return Result<Json>::ok(Json{{"patches", std::move(list)}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Speed
// ---------------------------------------------------------------------------

void ToolRegistry::registerSpeed() {
    add({"speed_load",
         "Inject the speed payload that matches the target's architecture. Safe to call again: an "
         "already-loaded payload is found rather than loaded twice.",
         emptySchema(),
         [this](const Json&) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             auto loaded = services_.speed().load();
             if (!loaded) {
                 return Result<Json>::fail(loaded.error(), loaded.code());
             }
             return Result<Json>::ok(Json{{"loaded", true}});
         },
         true});

    add({"speed_set_scale",
         "Set the rate of the clocks the target reads. Not a hack on the game but on its clock: "
         "everything a game does per frame is a delta multiplied by something, and it gets that "
         "delta by asking Windows the time twice.",
         objectSchema(Json{{"scale", prop("number", "Multiplier, between 0.05 and 20.")}}, {"scale"}),
         [this](const Json& args) {
             const auto scale = optionalDouble(args, "scale", 1.0);
             auto applied = services_.speed().setScale(scale);
             if (!applied) {
                 return Result<Json>::fail(applied.error(), applied.code());
             }
             return Result<Json>::ok(Json{{"scale", scale}});
         },
         true});

    add({"speed_reset",
         "Back to normal speed, with every patched import restored. The payload stays loaded.",
         emptySchema(),
         [this](const Json&) {
             auto reset = services_.speed().reset();
             if (!reset) {
                 return Result<Json>::fail(reset.error(), reset.code());
             }
             return Result<Json>::ok(Json{{"scale", 1.0}});
         },
         true});

    add({"speed_status",
         "Whether the payload is loaded and running, and how many imports it actually redirected. "
         "Zero hooked imports after a successful injection is the honest and important case: this "
         "program does not ask for the time through its import table.",
         emptySchema(),
         [this](const Json&) {
             const auto status = services_.speed().status();
             return Result<Json>::ok(Json{{"loaded", status.loaded},
                                          {"running", status.running},
                                          {"requested", status.requested},
                                          {"applied", status.applied},
                                          {"hooked_imports", status.hookedImports}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Projects
//
// These are marked non-mutating because they marshal onto the UI thread
// themselves, inside UiApp. Marking them mutating as well would queue a request
// from the UI thread onto the UI thread and wait for it, which is a deadlock.
// ---------------------------------------------------------------------------

void ToolRegistry::registerProject() {
    add({"project_save",
         "Save the address list, symbols, scripts and structures to an .iretable file.",
         objectSchema(Json{{"path", prop("string", "Where to write it.")}}, {"path"}),
         [](const Json& args) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             const auto path = requireString(args, "path");
             if (!path) {
                 return Result<Json>::fail(path.error());
             }
             auto saved = commands->saveProject(path.value());
             if (!saved) {
                 return Result<Json>::fail(saved.error(), saved.code());
             }
             return Result<Json>::ok(Json{{"saved", path.value()}});
         },
         false});

    add({"project_load",
         "Load an .iretable file, replacing the address list, symbols, scripts and structures. Any "
         "script currently on is switched off first, so the target is put back as it was.",
         objectSchema(Json{{"path", prop("string", "The file to open.")}}, {"path"}),
         [](const Json& args) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             const auto path = requireString(args, "path");
             if (!path) {
                 return Result<Json>::fail(path.error());
             }
             auto loaded = commands->loadProject(path.value());
             if (!loaded) {
                 return Result<Json>::fail(loaded.error(), loaded.code());
             }
             return Result<Json>::ok(Json{{"loaded", path.value()}});
         },
         false});
}

// ---------------------------------------------------------------------------
// The window
//
// Non-mutating for the same reason as the project tools: every one of these
// blocks on the UI thread inside UiApp already.
// ---------------------------------------------------------------------------

void ToolRegistry::registerWindow() {
    add({"screenshot",
         "Write the window's back buffer to a PNG. This captures the window, not the screen, so "
         "nothing behind it can end up in the picture.",
         objectSchema(Json{{"path", prop("string", "Where to write the PNG.")}}, {"path"}),
         [](const Json& args) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             const auto path = requireString(args, "path");
             if (!path) {
                 return Result<Json>::fail(path.error());
             }
             auto captured = commands->screenshot(path.value());
             if (!captured) {
                 return Result<Json>::fail(captured.error(), captured.code());
             }
             return Result<Json>::ok(Json{{"written", path.value()}});
         },
         false});

    add({"select_panel",
         "Open a panel and bring it to the front, so the person at the window is looking at what "
         "you are working on.",
         objectSchema(Json{{"name", prop("string", "The panel's title exactly as it appears in the "
                                                   "View menu, e.g. \"Access Watch\".")}},
                      {"name"}),
         [](const Json& args) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             const auto name = requireString(args, "name");
             if (!name) {
                 return Result<Json>::fail(name.error());
             }
             auto selected = commands->selectPanel(name.value());
             if (!selected) {
                 return Result<Json>::fail(selected.error(), selected.code());
             }
             return Result<Json>::ok(Json{{"selected", name.value()}});
         },
         false});

    add({"set_layout",
         "Restore the shipped panel arrangement. \"default\" is the only name there is.",
         objectSchema(Json{{"name", prop("string", "\"default\".")}}, {"name"}),
         [](const Json& args) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             const auto name = requireString(args, "name");
             if (!name) {
                 return Result<Json>::fail(name.error());
             }
             auto applied = commands->setLayout(name.value());
             if (!applied) {
                 return Result<Json>::fail(applied.error(), applied.code());
             }
             return Result<Json>::ok(Json{{"layout", name.value()}});
         },
         false});

    add({"set_window_size",
         "Set the window's client size, between 320x240 and 8192x8192.",
         objectSchema(Json{{"width", prop("integer", "Client width in pixels.")},
                           {"height", prop("integer", "Client height in pixels.")}},
                      {"width", "height"}),
         [](const Json& args) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             const auto width = requireUint(args, "width");
             if (!width) {
                 return Result<Json>::fail(width.error());
             }
             const auto height = requireUint(args, "height");
             if (!height) {
                 return Result<Json>::fail(height.error());
             }
             auto resized = commands->setWindowSize(static_cast<int>(width.value()),
                                                    static_cast<int>(height.value()));
             if (!resized) {
                 return Result<Json>::fail(resized.error(), resized.code());
             }
             return Result<Json>::ok(Json{{"width", width.value()}, {"height", height.value()}});
         },
         false});

    add({"quit",
         "Close Pointer Lab. The usual exit path runs, so the session is autosaved as if the window "
         "had been closed by hand.",
         emptySchema(),
         [](const Json&) {
             auto* commands = window();
             if (commands == nullptr) {
                 return noWindow();
             }
             auto quitting = commands->quit();
             if (!quitting) {
                 return Result<Json>::fail(quitting.error(), quitting.code());
             }
             return Result<Json>::ok(Json{{"quitting", true}});
         },
         false});
}

} // namespace ire::mcp
