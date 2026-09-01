// The registry, and the tools closest to the target: process, memory, scan.
//
// The other two halves of the surface live in McpToolsAnalysis.cpp and
// McpToolsControl.cpp. Splitting them is the same decision that split UiApp: one
// file holding every handler would be several times the size of the next largest
// in the tree.

#include "mcp/McpInternal.h"
#include "mcp/McpTools.h"

#include "engine_scan/MemoryScanner.h"
#include "services/UiCommands.h"

#include <algorithm>

namespace ire::mcp {

namespace {

template <typename T>
using Result = infra::Result<T>;

// The most common failure by a wide margin, and the one whose stock message is
// worth having in one place: every tool that touches the target says the same
// sentence, so a caller learns the remedy once.
Result<Json> notAttached() {
    return Result<Json>::fail("No target process is attached. Call attach with a pid first.");
}

// A scan result set is a million rows on a bad day and each row holds three byte
// vectors, so the page size is capped rather than trusted from the caller. The
// cap is generous enough that a caller asking for "everything" gets a useful
// screenful and a total telling it how much it did not get.
constexpr std::size_t maxPageSize = 1000;

} // namespace

ToolRegistry::ToolRegistry(services::RuntimeServices& services) : services_(services) {
    registerTarget();
    registerMemory();
    registerScan();
    registerSymbols();
    registerPointers();
    registerAddressList();
    registerStructures();
    registerCode();
    registerBreakpoints();
    registerPatches();
    registerSpeed();
    registerProject();
    registerWindow();
}

void ToolRegistry::add(Tool tool) {
    byName_.emplace(tool.name, tools_.size());
    tools_.push_back(std::move(tool));
}

const Tool* ToolRegistry::find(const std::string& name) const {
    const auto found = byName_.find(name);
    if (found == byName_.end()) {
        return nullptr;
    }
    return &tools_[found->second];
}

infra::Result<Json> ToolRegistry::call(const std::string& name, const Json& arguments) {
    const Tool* tool = find(name);
    if (tool == nullptr) {
        return Result<Json>::fail("There is no tool called \"" + name + "\".");
    }
    // A caller that sends no arguments at all sends null, not {}. Normalising
    // here means every handler can index the object without checking first.
    const Json args = arguments.is_object() ? arguments : Json::object();

    if (!tool->mutating) {
        return tool->handler(args);
    }

    auto* commands = services::uiCommands();
    if (commands == nullptr) {
        // No window, so no other thread is mutating the engines and the server
        // runs one call at a time. Inline is then the whole of the contract --
        // this is the path the tests take.
        return tool->handler(args);
    }

    auto outcome = Result<Json>::fail("The tool did not run.");
    const auto ran = commands->runOnUiThread([&] { outcome = tool->handler(args); });
    if (!ran) {
        return Result<Json>::fail(ran.error());
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------

void ToolRegistry::registerTarget() {
    add({"processes",
         "List the running processes Pointer Lab can see, as pid and name.",
         emptySchema(),
         [this](const Json&) {
             auto processes = services_.platform().listProcesses();
             std::sort(processes.begin(), processes.end(),
                       [](const domain::ProcessInfo& a, const domain::ProcessInfo& b) {
                           return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                       });
             Json list = Json::array();
             for (const auto& process : processes) {
                 list.push_back(Json{{"pid", process.pid}, {"name", narrow(process.name)}});
             }
             return Result<Json>::ok(Json{{"processes", std::move(list)}});
         },
         false});

    add({"attach",
         "Attach to a process by pid. Everything else that touches the target needs this first. "
         "Attaching replaces any existing attachment.",
         objectSchema(Json{{"pid", prop("integer", "Process id, from the processes tool.")}}, {"pid"}),
         [this](const Json& args) {
             const auto pid = requireUint(args, "pid");
             if (!pid) {
                 return Result<Json>::fail(pid.error());
             }
             auto attached = services_.session().attach(static_cast<std::uint32_t>(pid.value()));
             if (!attached) {
                 return Result<Json>::fail(attached.error(), attached.code());
             }
             // Chain-backed address list entries point at wherever the previous
             // target had them; resolving now means the list is right before the
             // caller looks rather than up to half a second later.
             services_.addressList().resolvePointerChains();
             auto& session = services_.session();
             return Result<Json>::ok(Json{{"pid", session.pid()},
                                          {"name", narrow(session.processName())},
                                          {"bitness", domain::bitnessName(session.bitness())},
                                          // Surfaced rather than logged: with a read-only handle every
                                          // write, freeze and patch will fail one at a time, and a caller
                                          // that knows now can say so instead of discovering it per call.
                                          {"read_only", session.readOnly()}});
         },
         true});

    add({"detach",
         "Detach from the target process. Always succeeds, including when nothing is attached.",
         emptySchema(),
         [this](const Json&) {
             services_.session().detach();
             return Result<Json>::ok(Json{{"attached", false}});
         },
         true});

    add({"refresh",
         "Re-read the target's module and region lists. Needed to see a module the target loaded "
         "after you attached.",
         emptySchema(),
         [this](const Json&) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             services_.session().refresh();
             return Result<Json>::ok(Json{{"modules", services_.session().modules().size()},
                                          {"regions", services_.session().regions().size()}});
         },
         true});

    add({"session_info",
         "What is currently attached: pid, name, pointer width, and whether the handle is read-only "
         "or the process has exited.",
         emptySchema(),
         [this](const Json&) {
             auto& session = services_.session();
             if (!session.attached()) {
                 return Result<Json>::ok(Json{{"attached", false}});
             }
             return Result<Json>::ok(Json{{"attached", true},
                                          {"pid", session.pid()},
                                          {"name", narrow(session.processName())},
                                          {"bitness", domain::bitnessName(session.bitness())},
                                          {"pointer_size", session.pointerSize()},
                                          {"read_only", session.readOnly()},
                                          {"exited", session.exited()}});
         },
         false});

    add({"modules",
         "The target's loaded modules: name, base address and size. This is the snapshot taken at "
         "attach or the last refresh.",
         objectSchema(Json{{"filter", prop("string", "Case-insensitive substring of the module name.")}}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto filter = optionalString(args, "filter");
             Json list = Json::array();
             for (const auto& module : services_.session().modules()) {
                 auto name = narrow(module.name);
                 if (!filter.empty()) {
                     std::string haystack = name;
                     std::string needle = filter;
                     const auto fold = [](std::string& text) {
                         std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                             return static_cast<char>(std::tolower(c));
                         });
                     };
                     fold(haystack);
                     fold(needle);
                     if (haystack.find(needle) == std::string::npos) {
                         continue;
                     }
                 }
                 list.push_back(Json{{"name", std::move(name)},
                                     {"base", static_cast<std::uint64_t>(module.base)},
                                     {"base_hex", domain::toHex(module.base)},
                                     {"size", module.size}});
             }
             return Result<Json>::ok(Json{{"modules", std::move(list)}});
         },
         false});

    add({"regions",
         "The target's committed memory regions and their protection flags.",
         objectSchema(Json{{"writable_only", prop("boolean", "Only regions that can be written.")},
                           {"executable_only", prop("boolean", "Only regions that can be executed.")},
                           {"limit", prop("integer", "Maximum regions to return. Defaults to 200.")}}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const bool writableOnly = optionalBool(args, "writable_only");
             const bool executableOnly = optionalBool(args, "executable_only");
             const auto limit = static_cast<std::size_t>(optionalUint(args, "limit", 200));
             const auto regions = services_.session().regions();
             Json list = Json::array();
             std::size_t total = 0;
             for (const auto& region : regions) {
                 if ((writableOnly && !region.writable) || (executableOnly && !region.executable)) {
                     continue;
                 }
                 ++total;
                 if (list.size() >= limit) {
                     continue;
                 }
                 list.push_back(Json{{"base", static_cast<std::uint64_t>(region.base)},
                                     {"base_hex", domain::toHex(region.base)},
                                     {"size", region.size},
                                     {"readable", region.readable},
                                     {"writable", region.writable},
                                     {"executable", region.executable}});
             }
             return Result<Json>::ok(Json{{"regions", std::move(list)}, {"total", total}});
         },
         false});
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

void ToolRegistry::registerMemory() {
    add({"read",
         "Read and decode one typed value from the target.",
         objectSchema(Json{{"address", addressProp("Where to read.")},
                           {"type", enumProp(valueTypeNames(), "Value type. Defaults to i32.")}},
                      {"address"}),
         [this](const Json& args) {
             // Arguments that need no session are checked before the session is,
             // so a caller that got the type wrong hears about the type rather
             // than being sent to attach first and told about it on the next
             // round trip. An address *expression* does need the session, so it
             // is resolved after -- there the "attach first" message is the
             // useful one.
             const auto type = requireValueType(args, "type");
             if (!type) {
                 return Result<Json>::fail(type.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             // A variable-width type has no width of its own; one byte matches
             // what the Lua read() does, and read_bytes is the tool for more.
             const auto size = std::max<std::size_t>(domain::valueTypeSize(type.value()), 1);
             auto bytes = services_.session().readBytes(address.value(), size);
             if (!bytes) {
                 return Result<Json>::fail(bytes.error(), bytes.code());
             }
             // A short read is reported by the platform as success with a
             // shorter buffer, so the length has to be compared rather than
             // trusted -- otherwise half a value decodes as a whole one.
             if (bytes.value().size() < size) {
                 return Result<Json>::fail("Short read: " + std::to_string(bytes.value().size()) + " of " +
                                           std::to_string(size) + " bytes at " + domain::toHex(address.value()) +
                                           ".");
             }
             Json out = addressJson(address.value());
             out["type"] = domain::valueTypeName(type.value());
             out["value"] = decodeValue(type.value(), bytes.value());
             out["bytes"] = domain::bytesToHex(bytes.value(), false);
             return Result<Json>::ok(std::move(out));
         },
         false});

    add({"write",
         "Write one typed value to the target. This changes the memory of a live process.",
         objectSchema(Json{{"address", addressProp("Where to write.")},
                           {"type", enumProp(valueTypeNames(), "Value type. Defaults to i32.")},
                           {"value", prop("string", "The value. A number, or a string for bytes/str/wstr. "
                                                    "Hex for bytes is lenient: \"48 8B 05\" and \"488b05\" "
                                                    "are the same input.")}},
                      {"address", "value"}),
         [this](const Json& args) {
             const auto type = requireValueType(args, "type");
             if (!type) {
                 return Result<Json>::fail(type.error());
             }
             const auto text = requireValueText(args, "value");
             if (!text) {
                 return Result<Json>::fail(text.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             const auto parsed = domain::parseScanValue(type.value(), text.value());
             if (!parsed) {
                 return Result<Json>::fail("\"" + text.value() + "\" is not a valid " +
                                           domain::valueTypeName(type.value()) + " value.");
             }
             auto written = services_.session().writeBytes(address.value(), parsed->bytes);
             if (!written) {
                 return Result<Json>::fail(written.error(), written.code());
             }
             Json out = addressJson(address.value());
             out["written"] = parsed->bytes.size();
             out["bytes"] = domain::bytesToHex(parsed->bytes, false);
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"read_bytes",
         "Read raw bytes from the target as an uppercase hex string. A partial read is not an error: "
         "check the returned length.",
         objectSchema(Json{{"address", addressProp("Where to read.")},
                           {"size", prop("integer", "How many bytes. Capped at 4096.")}},
                      {"address", "size"}),
         [this](const Json& args) {
             const auto size = requireUint(args, "size");
             if (!size) {
                 return Result<Json>::fail(size.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             // Capped because the whole reply is one JSON string in one HTTP
             // response, and a caller asking for a megabyte gets three megabytes
             // of hex it has no way to use.
             const auto wanted = std::min<std::size_t>(static_cast<std::size_t>(size.value()), 4096);
             if (wanted == 0) {
                 return Result<Json>::fail("size must be at least 1.");
             }
             auto bytes = services_.session().readBytes(address.value(), wanted);
             if (!bytes) {
                 return Result<Json>::fail(bytes.error(), bytes.code());
             }
             Json out = addressJson(address.value());
             out["requested"] = wanted;
             out["length"] = bytes.value().size();
             out["hex"] = domain::bytesToHex(bytes.value(), false);
             return Result<Json>::ok(std::move(out));
         },
         false});

    add({"write_bytes",
         "Write raw bytes to the target from a hex string. This changes the memory of a live process.",
         objectSchema(Json{{"address", addressProp("Where to write.")},
                           {"hex", prop("string", "Bytes as hex. Parsed leniently: \"48 8B 05\", "
                                                  "\"488b05\" and \"48-8B-05\" are the same input.")}},
                      {"address", "hex"}),
         [this](const Json& args) {
             const auto hex = requireString(args, "hex");
             if (!hex) {
                 return Result<Json>::fail(hex.error());
             }
             const auto bytes = domain::parseHexBytes(hex.value());
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
             auto written = services_.session().writeBytes(address.value(), bytes);
             if (!written) {
                 return Result<Json>::fail(written.error(), written.code());
             }
             Json out = addressJson(address.value());
             out["written"] = bytes.size();
             return Result<Json>::ok(std::move(out));
         },
         true});

    add({"read_pointer",
         "Read one pointer at the target's own width -- four bytes on a 32-bit target, eight on a "
         "64-bit one.",
         objectSchema(Json{{"address", addressProp("Where the pointer is.")}}, {"address"}),
         [this](const Json& args) {
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto address = requireAddress(services_, args, "address");
             if (!address) {
                 return Result<Json>::fail(address.error(), address.code());
             }
             auto pointer = services_.session().readPointer(address.value());
             if (!pointer) {
                 return Result<Json>::fail(pointer.error(), pointer.code());
             }
             Json out = addressJson(address.value());
             out["points_to"] = static_cast<std::uint64_t>(pointer.value());
             out["points_to_hex"] = domain::toHex(pointer.value());
             return Result<Json>::ok(std::move(out));
         },
         false});
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void ToolRegistry::registerScan() {
    add({"scan_first",
         "Start a first scan. Returns immediately; the scan runs in the background, so poll "
         "scan_status until running is false and then read scan_results. A mode that compares "
         "against an earlier scan records a baseline instead of filtering.",
         objectSchema(Json{{"mode", enumProp(scanModeNames(), "Scan mode, as shown in the Scanner panel.")},
                           {"type", enumProp(valueTypeNames(), "Value type. Defaults to i32.")},
                           {"value", prop("string", "The value to look for. Required for the modes that "
                                                    "compare against one.")},
                           {"value2", prop("string", "Upper bound, for \"Value between\".")},
                           {"case_insensitive", prop("boolean", "Fold case, for str and wstr scans.")}},
                      {"mode"}),
         [this](const Json& args) {
             const auto mode = requireScanMode(args, "mode");
             if (!mode) {
                 return Result<Json>::fail(mode.error());
             }
             const auto type = requireValueType(args, "type");
             if (!type) {
                 return Result<Json>::fail(type.error());
             }
             // Refused up front rather than run: ordering has no meaning for a
             // byte pattern or a string, and such a scan can only ever return
             // nothing, which reads as "not found" rather than "not asked".
             if (!engine_scan::modeSupportsType(mode.value(), type.value())) {
                 return Result<Json>::fail(std::string(domain::scanModeName(mode.value())) +
                                           " cannot be used with " + domain::valueTypeName(type.value()) +
                                           ": ordering has no meaning for it.");
             }
             auto value = buildScanValue(args, mode.value(), type.value());
             if (!value) {
                 return Result<Json>::fail(value.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             services_.scanJob().startFirst(mode.value(), std::move(value.value()));
             return Result<Json>::ok(Json{
                 {"started", true},
                 {"mode", domain::scanModeName(mode.value())},
                 {"type", domain::valueTypeName(type.value())},
                 {"baseline_only", engine_scan::modeNeedsBaseline(mode.value())}});
         },
         true});

    add({"scan_next",
         "Narrow the previous scan's results. The value type is the one the first scan used and "
         "cannot be changed partway through a chain.",
         objectSchema(Json{{"mode", enumProp(scanModeNames(), "Scan mode.")},
                           {"value", prop("string", "The value, for the modes that compare against one.")},
                           {"value2", prop("string", "Upper bound, for \"Value between\".")},
                           {"case_insensitive", prop("boolean", "Fold case, for str and wstr scans.")}},
                      {"mode"}),
         [this](const Json& args) {
             const auto mode = requireScanMode(args, "mode");
             if (!mode) {
                 return Result<Json>::fail(mode.error());
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             auto previous = services_.scanJob().results();
             if (previous.empty()) {
                 return Result<Json>::fail("There are no results to narrow. Run scan_first first.");
             }
             const auto type = services_.scanJob().valueType();
             if (!engine_scan::modeSupportsType(mode.value(), type)) {
                 return Result<Json>::fail(std::string(domain::scanModeName(mode.value())) +
                                           " cannot be used with " + domain::valueTypeName(type) +
                                           ": ordering has no meaning for it.");
             }
             auto value = buildScanValue(args, mode.value(), type);
             if (!value) {
                 return Result<Json>::fail(value.error());
             }
             const auto count = previous.size();
             services_.scanJob().startNext(mode.value(), std::move(value.value()), std::move(previous));
             return Result<Json>::ok(Json{{"started", true},
                                          {"mode", domain::scanModeName(mode.value())},
                                          {"type", domain::valueTypeName(type)},
                                          {"narrowing", count}});
         },
         true});

    add({"scan_status",
         "How the scan in flight is doing. Never fails.",
         emptySchema(),
         [this](const Json&) {
             const auto progress = services_.scanJob().progress();
             return Result<Json>::ok(Json{{"running", progress.running},
                                          {"fraction", progress.fraction},
                                          {"results", progress.results},
                                          {"status", progress.status},
                                          // Distinct from cancelled: it means there were more matches
                                          // than the result cap allowed, so the sweep stopped early.
                                          {"truncated", progress.truncated},
                                          {"type", domain::valueTypeName(services_.scanJob().valueType())}});
         },
         false});

    add({"scan_results",
         "A page of the current scan results, with the total held so you can tell a partial view "
         "from a complete one.",
         objectSchema(Json{{"offset", prop("integer", "First result to return. Defaults to 0.")},
                           {"limit", prop("integer", "How many to return. Defaults to 100, capped at 1000.")}}),
         [this](const Json& args) {
             const auto offset = static_cast<std::size_t>(optionalUint(args, "offset", 0));
             const auto limit = std::min<std::size_t>(
                 static_cast<std::size_t>(optionalUint(args, "limit", 100)), maxPageSize);
             const auto type = services_.scanJob().valueType();
             const auto total = services_.scanJob().resultCount();
             // copyRange rather than results(): the full set is three vectors per
             // row, and copying a million of them holds the lock the scan worker
             // needs to append its next batch.
             const auto page = services_.scanJob().copyRange(offset, limit);
             Json list = Json::array();
             for (const auto& result : page) {
                 Json row = addressJson(result.address);
                 row["value"] = decodeValue(type, result.current);
                 row["hex"] = domain::bytesToHex(result.current, false);
                 if (!result.previous.empty()) {
                     row["previous"] = decodeValue(type, result.previous);
                 }
                 row["static"] = engine_symbols::SymbolTable::isStatic(services_.session(), result.address);
                 list.push_back(std::move(row));
             }
             return Result<Json>::ok(Json{{"results", std::move(list)},
                                          {"offset", offset},
                                          {"total", total},
                                          {"type", domain::valueTypeName(type)}});
         },
         false});

    add({"scan_cancel",
         "Stop the scan in flight. Returns once it has actually stopped.",
         emptySchema(),
         [this](const Json&) {
             services_.scanJob().cancel();
             return Result<Json>::ok(Json{{"cancelled", true}});
         },
         true});

    add({"scan_set_options",
         "Set the limits the next scan runs under. These persist until changed.",
         objectSchema(Json{{"max_results", prop("integer", "Stop after this many matches. Defaults to 1000000.")},
                           {"writable_only", prop("boolean", "Only scan regions that can be written.")},
                           {"executable_only", prop("boolean", "Only scan regions that can be executed.")},
                           {"float_epsilon", prop("number", "Tolerance for exact f32/f64 matches. Comparing "
                                                            "floats by their bytes almost never finds "
                                                            "anything.")}}),
         [this](const Json& args) {
             engine_scan::ScanOptions options;
             options.maxResults = static_cast<std::size_t>(optionalUint(args, "max_results", 1000000));
             options.writableOnly = optionalBool(args, "writable_only");
             options.executableOnly = optionalBool(args, "executable_only");
             options.floatEpsilon = optionalDouble(args, "float_epsilon", 0.0001);
             services_.scanJob().setOptions(options);
             return Result<Json>::ok(Json{{"max_results", options.maxResults},
                                          {"writable_only", options.writableOnly},
                                          {"executable_only", options.executableOnly},
                                          {"float_epsilon", options.floatEpsilon}});
         },
         true});

    add({"find_pattern",
         "Search a range for a byte pattern, synchronously. '?' and '??' are wildcards, as in "
         "\"48 8B ?? 24\". Returns every match up to the cap, because a pattern that matches twice "
         "is not specific enough to rely on.",
         objectSchema(Json{{"start", addressProp("Where to start searching.")},
                           {"size", prop("integer", "How many bytes to search.")},
                           {"pattern", prop("string", "The byte pattern, with optional '?' wildcards.")},
                           {"max_results", prop("integer", "Maximum matches. Defaults to 64.")}},
                      {"start", "size", "pattern"}),
         [this](const Json& args) {
             const auto size = requireUint(args, "size");
             if (!size) {
                 return Result<Json>::fail(size.error());
             }
             const auto text = requireString(args, "pattern");
             if (!text) {
                 return Result<Json>::fail(text.error());
             }
             const auto pattern = domain::parseHexPattern(text.value());
             if (!pattern) {
                 return Result<Json>::fail("\"" + text.value() + "\" is not a byte pattern.");
             }
             if (!services_.session().attached()) {
                 return notAttached();
             }
             const auto start = requireAddress(services_, args, "start");
             if (!start) {
                 return Result<Json>::fail(start.error(), start.code());
             }
             const auto cap = std::min<std::size_t>(
                 static_cast<std::size_t>(optionalUint(args, "max_results", 64)), maxPageSize);
             const auto matches = engine_scan::findPattern(services_.session(), start.value(),
                                                           static_cast<std::size_t>(size.value()),
                                                           *pattern, cap);
             Json list = Json::array();
             for (const auto address : matches) {
                 list.push_back(addressJson(address));
             }
             return Result<Json>::ok(Json{{"matches", std::move(list)}, {"count", matches.size()}});
         },
         false});
}

} // namespace ire::mcp
