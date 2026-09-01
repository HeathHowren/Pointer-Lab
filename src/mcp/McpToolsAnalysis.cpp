// The tools that turn a found address into something durable: names for it,
// pointer chains that survive a restart, the tracked address list, and the
// structure laid over it.

#include "mcp/McpInternal.h"
#include "mcp/McpTools.h"

#include "engine_pointer/PointerScanner.h"

#include <algorithm>

namespace ire::mcp {

namespace {

template <typename T>
using Result = infra::Result<T>;

Result<Json> notAttached() {
    return Result<Json>::fail("No target process is attached. Call attach with a pid first.");
}

Json chainJson(const domain::PointerChain& chain) {
    Json offsets = Json::array();
    for (const auto offset : chain.offsets) {
        offsets.push_back(static_cast<std::int64_t>(offset));
    }
    return Json{{"module", narrow(chain.moduleName)},
                {"module_offset", static_cast<std::uint64_t>(chain.moduleOffset)},
                {"module_offset_hex", domain::toHex(chain.moduleOffset)},
                {"offsets", std::move(offsets)}};
}

// A chain out of the arguments a tool was given. Shared by resolve_chain and
// add_chain_address, which describe one the same way.
Result<domain::PointerChain> buildChain(const Json& args) {
    domain::PointerChain chain;
    chain.moduleName = domain::widen(optionalString(args, "module"));
    const auto base = requireUint(args, "module_offset");
    if (!base) {
        return Result<domain::PointerChain>::fail(base.error());
    }
    chain.moduleOffset = static_cast<std::uintptr_t>(base.value());
    auto offsets = requireOffsets(args, "offsets");
    if (!offsets) {
        return Result<domain::PointerChain>::fail(offsets.error());
    }
    chain.offsets = std::move(offsets.value());
    if (!chain.valid()) {
        return Result<domain::PointerChain>::fail(
            "A chain needs either a module name or a non-zero module_offset to start from.");
    }
    return Result<domain::PointerChain>::ok(std::move(chain));
}

} // namespace

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

void ToolRegistry::registerSymbols() {
    add({"resolve",
         "Resolve an address expression to an address. Accepts a user symbol, a module name, "
         "module+hex, module.export, module!export, or plain hex -- each followed by any number of "
         "+hex or -hex terms. Offsets are always hexadecimal.",
         objectSchema(Json{{"expression", prop("string", "The expression, e.g. \"client.dll+0x4A2C10\".")}},
                      {"expression"}),
         [this](const Json& args) {
             const auto expression = requireString(args, "expression");
             if (!expression) {
                 return Result<Json>::fail(expression.error());
             }
             auto address = services_.symbols().resolve(services_.session(), expression.value());
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             Json out = addressJson(address.value());
             out["expression"] = expression.value();
             out["static"] = engine_symbols::SymbolTable::isStatic(services_.session(), address.value());
             return Result<Json>::ok(std::move(out));
         },
         false});

    add({"describe",
         "The most specific name for an address: a user symbol that names it exactly, else "
         "module.dll+0x1234 when it is inside a loaded module, else nothing. An address with a name "
         "is one worth writing down, because it is at the same place after a restart.",
         objectSchema(Json{{"address", addressProp("The address to name.")}}, {"address"}),
         [this](const Json& args) {
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             const auto description = services_.symbols().describe(services_.session(), address.value());
             Json out = addressJson(address.value());
             out["description"] = description;
             out["static"] = engine_symbols::SymbolTable::isStatic(services_.session(), address.value());
             if (const auto module = engine_symbols::SymbolTable::moduleAt(services_.session(), address.value())) {
                 out["module"] = narrow(module->name);
                 out["module_offset_hex"] = domain::toHex(address.value() - module->base);
             }
             return Result<Json>::ok(std::move(out));
         },
         false});

    add({"symbol_define",
         "Give a name to an address or an expression. An expression is stored rather than the "
         "address it resolves to now, so the symbol still points at the right thing after the "
         "target restarts.",
         objectSchema(Json{{"name", prop("string", "The symbol name.")},
                           {"expression", prop("string", "What it names, e.g. \"client.dll+0x4A2C10\".")}},
                      {"name", "expression"}),
         [this](const Json& args) {
             const auto name = requireString(args, "name");
             if (!name) {
                 return Result<Json>::fail(name.error());
             }
             const auto expression = requireString(args, "expression");
             if (!expression) {
                 return Result<Json>::fail(expression.error());
             }
             auto address = services_.symbols().define(services_.session(), name.value(), expression.value());
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             Json out = addressJson(address.value());
             out["name"] = name.value();
             out["expression"] = expression.value();
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"symbol_undefine",
         "Forget a user symbol.",
         objectSchema(Json{{"name", prop("string", "The symbol name.")}}, {"name"}),
         [this](const Json& args) {
             const auto name = requireString(args, "name");
             if (!name) {
                 return Result<Json>::fail(name.error());
             }
             if (!services_.symbols().undefine(name.value())) {
                 return Result<Json>::fail("There is no symbol called \"" + name.value() + "\".");
             }
             return Result<Json>::ok(Json{{"removed", name.value()}});
         },
         true});

    add({"symbols_list",
         "Every user symbol, with what it was defined as and where it resolves now.",
         emptySchema(),
         [this](const Json&) {
             Json list = Json::array();
             for (const auto& symbol : services_.symbols().symbols()) {
                 list.push_back(Json{{"name", symbol.name},
                                     {"expression", symbol.expression},
                                     {"address", static_cast<std::uint64_t>(symbol.address)},
                                     {"hex", domain::toHex(symbol.address)}});
             }
             return Result<Json>::ok(Json{{"symbols", std::move(list)}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Pointer chains
// ---------------------------------------------------------------------------

void ToolRegistry::registerPointers() {
    add({"resolve_chain",
         "Walk a pointer chain and return the address it currently points at. Each offset "
         "dereferences the current address and then adds the offset, so the final offset is added "
         "rather than dereferenced -- the address returned is where the value lives.",
         objectSchema(Json{{"module", prop("string", "Module the chain starts in, e.g. \"client.dll\". "
                                                     "Omit for an absolute base, which will not survive "
                                                     "a restart.")},
                           {"module_offset", prop("integer", "Offset of the base within the module.")},
                           {"offsets", Json{{"type", "array"},
                                            {"items", Json{{"type", "integer"}}},
                                            {"description", "Offsets to walk, in order."}}}},
                      {"module_offset"}),
         [this](const Json& args) {
             // The chain is checked before the session, so a caller that wrote
             // an offset wrong hears about the offset rather than being sent to
             // attach and told about it on the next round trip.
             auto chain = buildChain(args);
             if (!chain) {
                 return Result<Json>::fail(chain.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             auto address = engine_pointer::resolveChain(services_.session(), chain.value());
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             Json out = addressJson(address.value());
             out["chain"] = chainJson(chain.value());
             return Result<Json>::ok(std::move(out));
         },
         false});

    add({"pointer_scan_start",
         "Find pointer chains that lead to an address. Runs in the background; poll "
         "pointer_scan_status. The chains a first scan finds merely pointed the right way once -- "
         "restart the target, find the value again, and use pointer_scan_filter to keep only the "
         "ones that still track it.",
         objectSchema(Json{{"target", addressProp("The address chains should lead to.")},
                           {"max_depth", prop("integer", "How many dereferences deep. Defaults to 3.")},
                           {"max_offset", prop("integer", "Largest offset to consider. Defaults to 0x1000.")},
                           {"max_results", prop("integer", "Stop after this many chains. Defaults to 10000.")}},
                      {"target"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto target = requireAddress(services_, args, "target");
             if (!target) {
                 return Result<Json>::fail(target.error(), target.code());
             }
             engine_pointer::PointerScanOptions options;
             options.target = target.value();
             options.maxDepth = static_cast<std::uint32_t>(optionalUint(args, "max_depth", 3));
             options.maxOffset = static_cast<std::uint32_t>(optionalUint(args, "max_offset", 0x1000));
             options.maxResults = static_cast<std::size_t>(optionalUint(args, "max_results", 10000));
             services_.pointerScanJob().start(options);
             return Result<Json>::ok(Json{{"started", true},
                                          {"target", static_cast<std::uint64_t>(target.value())},
                                          {"target_hex", domain::toHex(target.value())},
                                          {"max_depth", options.maxDepth}});
         },
         true});

    add({"pointer_scan_filter",
         "Keep only the chains that still resolve to a new address. This is the half of the "
         "workflow that separates a real chain from a coincidence, and it costs a few reads per "
         "chain rather than another sweep.",
         objectSchema(Json{{"target", addressProp("Where the value lives now.")}}, {"target"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto target = requireAddress(services_, args, "target");
             if (!target) {
                 return Result<Json>::fail(target.error(), target.code());
             }
             services_.pointerScanJob().filter(target.value());
             return Result<Json>::ok(Json{{"started", true},
                                          {"target", static_cast<std::uint64_t>(target.value())},
                                          {"target_hex", domain::toHex(target.value())}});
         },
         true});

    add({"pointer_scan_status",
         "How the pointer scan in flight is doing.",
         emptySchema(),
         [this](const Json&) {
             const auto progress = services_.pointerScanJob().progress();
             return Result<Json>::ok(Json{{"running", progress.running},
                                          {"fraction", progress.fraction},
                                          {"results", progress.results},
                                          {"status", progress.status}});
         },
         false});

    add({"pointer_scan_results",
         "A page of the pointer chains found.",
         objectSchema(Json{{"offset", prop("integer", "First chain to return. Defaults to 0.")},
                           {"limit", prop("integer", "How many. Defaults to 50, capped at 500.")}}),
         [this](const Json& args) {
             const auto offset = static_cast<std::size_t>(optionalUint(args, "offset", 0));
             const auto limit = std::min<std::size_t>(
                 static_cast<std::size_t>(optionalUint(args, "limit", 50)), 500);
             const auto chains = services_.pointerScanJob().results();
             Json list = Json::array();
             for (std::size_t i = offset; i < chains.size() && list.size() < limit; ++i) {
                 list.push_back(chainJson(chains[i]));
             }
             return Result<Json>::ok(Json{{"chains", std::move(list)},
                                          {"offset", offset},
                                          {"total", chains.size()}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Address list
// ---------------------------------------------------------------------------

void ToolRegistry::registerAddressList() {
    add({"add_address",
         "Track an address in the address list, where it appears in the window alongside anything "
         "the person is tracking by hand.",
         objectSchema(Json{{"address", addressProp("The address to track.")},
                           {"type", enumProp(valueTypeNames(), "Value type. Defaults to i32.")},
                           {"description", prop("string", "What it is. Defaults to \"MCP entry\".")},
                           {"group", prop("string", "Group heading. Defaults to \"MCP\".")}},
                      {"address"}),
         [this](const Json& args) {
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             const auto type = requireValueType(args, "type");
             if (!type) {
                 return Result<Json>::fail(type.error());
             }
             const auto id = services_.addressList().add(
                 address.value(), type.value(), optionalString(args, "description", "MCP entry"),
                 optionalString(args, "group", "MCP"));
             Json out = addressJson(address.value());
             out["id"] = id;
             out["type"] = domain::valueTypeName(type.value());
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"add_chain_address",
         "Track a pointer chain rather than a fixed address. The entry re-resolves as the target "
         "runs, so it keeps pointing at the value across a restart -- which a fixed address does "
         "not.",
         objectSchema(Json{{"module", prop("string", "Module the chain starts in.")},
                           {"module_offset", prop("integer", "Offset of the base within the module.")},
                           {"offsets", Json{{"type", "array"},
                                            {"items", Json{{"type", "integer"}}},
                                            {"description", "Offsets to walk, in order."}}},
                           {"type", enumProp(valueTypeNames(), "Value type. Defaults to i32.")},
                           {"description", prop("string", "What it is.")},
                           {"group", prop("string", "Group heading. Defaults to \"MCP\".")}},
                      {"module_offset"}),
         [this](const Json& args) {
             auto chain = buildChain(args);
             if (!chain) {
                 return Result<Json>::fail(chain.error());
             }
             const auto type = requireValueType(args, "type");
             if (!type) {
                 return Result<Json>::fail(type.error());
             }
             const auto id = services_.addressList().addChain(
                 chain.value(), type.value(), optionalString(args, "description", "MCP entry"),
                 optionalString(args, "group", "MCP"));
             return Result<Json>::ok(Json{{"id", id},
                                          {"chain", chainJson(chain.value())},
                                          {"type", domain::valueTypeName(type.value())}});
         },
         true});

    add({"list_addresses",
         "Everything in the address list, with each entry's current value.",
         emptySchema(),
         [this](const Json&) {
             const auto entries = services_.session().addressList().snapshot();
             const bool attached = services_.session().attached();
             Json list = Json::array();
             for (const auto& entry : entries) {
                 Json row{{"id", entry.id},
                          {"address", static_cast<std::uint64_t>(entry.address)},
                          {"hex", domain::toHex(entry.address)},
                          {"type", domain::valueTypeName(entry.type)},
                          {"description", entry.description},
                          {"group", entry.group},
                          {"frozen", entry.frozen},
                          {"resolved", entry.resolved}};
                 if (entry.hotkey.empty()) {
                     row["hotkey"] = nullptr;
                 } else {
                     row["hotkey"] = entry.hotkey;
                 }
                 if (entry.chain) {
                     row["chain"] = chainJson(*entry.chain);
                 }
                 if (attached && entry.resolved) {
                     const auto size = std::max<std::size_t>(domain::valueTypeSize(entry.type), 1);
                     if (auto bytes = services_.session().readBytes(entry.address, size)) {
                         row["value"] = decodeValue(entry.type, bytes.value());
                     }
                 }
                 list.push_back(std::move(row));
             }
             return Result<Json>::ok(Json{{"entries", std::move(list)},
                                          {"revision", services_.session().addressList().revision()}});
         },
         false});

    add({"remove_address",
         "Remove an entry from the address list.",
         objectSchema(Json{{"id", prop("integer", "Entry id, from list_addresses.")}}, {"id"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             if (!services_.addressList().remove(id.value())) {
                 return Result<Json>::fail("There is no address list entry with id " +
                                           std::to_string(id.value()) + ".");
             }
             return Result<Json>::ok(Json{{"removed", id.value()}});
         },
         true});

    add({"set_frozen",
         "Freeze or unfreeze an entry. A frozen entry is written back to the target twenty times a "
         "second, which is what holds a value against a program trying to change it.",
         objectSchema(Json{{"id", prop("integer", "Entry id, from list_addresses.")},
                           {"frozen", prop("boolean", "True to freeze, false to release.")}},
                      {"id", "frozen"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             const bool frozen = optionalBool(args, "frozen", true);
             if (!services_.addressList().setFrozen(id.value(), frozen)) {
                 return Result<Json>::fail("There is no address list entry with id " +
                                           std::to_string(id.value()) + ".");
             }
             return Result<Json>::ok(Json{{"id", id.value()}, {"frozen", frozen}});
         },
         true});

    add({"update_value",
         "Write a new value to an address list entry, in the entry's own type. This writes to the "
         "target immediately, and becomes the freeze value if the entry is frozen.",
         objectSchema(Json{{"id", prop("integer", "Entry id, from list_addresses.")},
                           {"value", prop("string", "The new value.")}},
                      {"id", "value"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             const auto entries = services_.session().addressList().snapshot();
             const auto found = std::find_if(entries.begin(), entries.end(),
                                             [&id](const domain::AddressEntry& entry) {
                                                 return entry.id == id.value();
                                             });
             if (found == entries.end()) {
                 return Result<Json>::fail("There is no address list entry with id " +
                                           std::to_string(id.value()) + ".");
             }
             const auto text = requireValueText(args, "value");
             if (!text) {
                 return Result<Json>::fail(text.error());
             }
             const auto parsed = domain::parseScanValue(found->type, text.value());
             if (!parsed) {
                 return Result<Json>::fail("\"" + text.value() + "\" is not a valid " +
                                           domain::valueTypeName(found->type) + " value.");
             }
             if (!services_.addressList().updateValue(id.value(), parsed->bytes)) {
                 return Result<Json>::fail("The value could not be written to " +
                                           domain::toHex(found->address) + ".");
             }
             return Result<Json>::ok(Json{{"id", id.value()},
                                          {"address", static_cast<std::uint64_t>(found->address)},
                                          {"hex", domain::toHex(found->address)},
                                          {"written", parsed->bytes.size()}});
         },
         true});
}

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------

void ToolRegistry::registerStructures() {
    add({"struct_add",
         "Create a named structure layout.",
         objectSchema(Json{{"name", prop("string", "What to call it.")}}, {"name"}),
         [this](const Json& args) {
             const auto name = requireString(args, "name");
             if (!name) {
                 return Result<Json>::fail(name.error());
             }
             const auto id = services_.dissector().add(name.value());
             return Result<Json>::ok(Json{{"id", id}, {"name", name.value()}});
         },
         true});

    add({"struct_list",
         "Every structure defined, with its fields.",
         emptySchema(),
         [this](const Json&) {
             Json list = Json::array();
             for (const auto& structure : services_.dissector().structures()) {
                 Json fields = Json::array();
                 for (const auto& field : structure.fields) {
                     fields.push_back(Json{{"offset", static_cast<std::int64_t>(field.offset)},
                                           {"offset_hex", domain::toHex(static_cast<std::uintptr_t>(field.offset))},
                                           {"type", domain::valueTypeName(field.type)},
                                           {"size", field.size()},
                                           {"name", field.name}});
                 }
                 list.push_back(Json{{"id", structure.id},
                                     {"name", structure.name},
                                     {"size", structure.sizeInBytes()},
                                     {"fields", std::move(fields)}});
             }
             return Result<Json>::ok(Json{{"structures", std::move(list)}});
         },
         false});

    add({"struct_set_fields",
         "Replace a structure's fields. Fields may not overlap: the display would have to pick "
         "which of two owns a byte, and whichever it picked would be wrong half the time.",
         objectSchema(
             Json{{"id", prop("integer", "Structure id.")},
                  {"fields", Json{{"type", "array"},
                                  {"description", "Each field is {offset, type, name, length}. length "
                                                  "applies only to bytes/str/wstr, whose width does not "
                                                  "come from the type."},
                                  {"items", Json{{"type", "object"}}}}}},
             {"id", "fields"}),
         [this](const Json& args) {
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             if (!has(args, "fields") || !args.at("fields").is_array()) {
                 return Result<Json>::fail("fields must be an array.");
             }
             std::vector<domain::StructureField> fields;
             for (const auto& entry : args.at("fields")) {
                 if (!entry.is_object()) {
                     return Result<Json>::fail("Every field must be an object.");
                 }
                 domain::StructureField field;
                 const auto offset = requireUint(entry, "offset");
                 if (!offset) {
                     return Result<Json>::fail(offset.error());
                 }
                 field.offset = static_cast<std::ptrdiff_t>(offset.value());
                 const auto type = requireValueType(entry, "type");
                 if (!type) {
                     return Result<Json>::fail(type.error());
                 }
                 field.type = type.value();
                 field.length = static_cast<std::size_t>(optionalUint(entry, "length", 0));
                 field.name = optionalString(entry, "name",
                                             domain::defaultFieldName(field.offset));
                 if (field.size() == 0) {
                     return Result<Json>::fail("A " + std::string(domain::valueTypeName(field.type)) +
                                               " field needs a length, because its width does not come "
                                               "from its type.");
                 }
                 fields.push_back(std::move(field));
             }
             auto applied = services_.dissector().setFields(id.value(), std::move(fields));
             if (!applied) {
                 return Result<Json>::fail(applied.error(), applied.code());
             }
             return Result<Json>::ok(Json{{"id", id.value()}});
         },
         true});

    add({"struct_read",
         "Read a structure at one or more addresses at once. Each row says whether every address "
         "holds the same bytes there -- putting instances side by side is the point, because the "
         "fields that differ between two objects are the ones that describe them.",
         objectSchema(Json{{"id", prop("integer", "Structure id.")},
                           {"addresses", Json{{"type", "array"},
                                              {"description", "Up to 8 addresses or expressions."},
                                              {"items", Json{{"type", Json::array({"integer", "string"})}}}}}},
                      {"id", "addresses"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             if (!has(args, "addresses") || !args.at("addresses").is_array()) {
                 return Result<Json>::fail("addresses must be an array.");
             }
             std::vector<std::uintptr_t> addresses;
             for (const auto& entry : args.at("addresses")) {
                 const Json wrapper{{"address", entry}};
                 const auto address = requireAddress(services_, wrapper, "address");
                 if (!address) {
                     return Result<Json>::fail(address.error(), address.code());
                 }
                 addresses.push_back(address.value());
             }
             if (addresses.empty()) {
                 return Result<Json>::fail("Give at least one address.");
             }
             auto snapshot = services_.dissector().read(id.value(), addresses);
             if (!snapshot) {
                 return Result<Json>::fail(snapshot.error(), snapshot.code());
             }
             Json rows = Json::array();
             for (const auto& row : snapshot.value().rows) {
                 Json cells = Json::array();
                 for (const auto& cell : row.cells) {
                     if (!cell.read) {
                         cells.push_back(Json(nullptr));
                         continue;
                     }
                     Json value{{"text", cell.text}};
                     if (!cell.annotation.empty()) {
                         value["annotation"] = cell.annotation;
                     }
                     cells.push_back(std::move(value));
                 }
                 rows.push_back(Json{{"offset", static_cast<std::int64_t>(row.field.offset)},
                                     {"offset_hex", domain::toHex(static_cast<std::uintptr_t>(row.field.offset))},
                                     {"type", domain::valueTypeName(row.field.type)},
                                     {"name", row.field.name},
                                     {"identical", row.identical},
                                     {"cells", std::move(cells)}});
             }
             Json addressList = Json::array();
             for (const auto address : snapshot.value().addresses) {
                 addressList.push_back(addressJson(address));
             }
             return Result<Json>::ok(Json{{"id", id.value()},
                                          {"addresses", std::move(addressList)},
                                          {"unreadable", snapshot.value().unreadable},
                                          {"rows", std::move(rows)}});
         },
         false});

    add({"struct_guess",
         "Fill a structure in from what is actually at those addresses, one field per slot, typed "
         "by what the bytes look like. Replaces any existing fields. This is a guess and should be "
         "treated as one; it exists because naming forty slots by hand before you know which matter "
         "is how people give up on structures.",
         objectSchema(Json{{"id", prop("integer", "Structure id.")},
                           {"addresses", Json{{"type", "array"},
                                              {"description", "Up to 8 addresses or expressions."},
                                              {"items", Json{{"type", Json::array({"integer", "string"})}}}}},
                           {"size", prop("integer", "How many bytes to lay out. Defaults to 0x400.")}},
                      {"id", "addresses"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto id = requireUint(args, "id");
             if (!id) {
                 return Result<Json>::fail(id.error());
             }
             if (!has(args, "addresses") || !args.at("addresses").is_array()) {
                 return Result<Json>::fail("addresses must be an array.");
             }
             std::vector<std::uintptr_t> addresses;
             for (const auto& entry : args.at("addresses")) {
                 const Json wrapper{{"address", entry}};
                 const auto address = requireAddress(services_, wrapper, "address");
                 if (!address) {
                     return Result<Json>::fail(address.error(), address.code());
                 }
                 addresses.push_back(address.value());
             }
             const auto size = static_cast<std::size_t>(
                 optionalUint(args, "size", engine_struct::Dissector::defaultSize));
             auto created = services_.dissector().guess(id.value(), addresses, size);
             if (!created) {
                 return Result<Json>::fail(created.error(), created.code());
             }
             return Result<Json>::ok(Json{{"id", id.value()}, {"fields", created.value()}});
         },
         true});
}

} // namespace ire::mcp
