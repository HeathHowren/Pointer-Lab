#pragma once

// Shared between the MCP tool translation units.
//
// The registry is one class split across several files, one group of engines
// each -- the same arrangement as ui/UiInternal.h, and for the same reason: a
// single file holding every handler would be the largest in the tree by a wide
// margin. These helpers are `inline` in a named namespace rather than static in
// an anonymous one so a file that happens not to use one does not trip /W4 /WX
// over an unused function.

#include "domain/Domain.h"
#include "engine_symbols/SymbolTable.h"
#include "infra/Result.h"
#include "mcp/McpTools.h"
#include "services/RuntimeServices.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ire::mcp {

// ---------------------------------------------------------------------------
// Schema construction
//
// Written out rather than generated, because the description strings are the
// only documentation the model gets. A tool whose schema says "address" and
// nothing else will be called with a decimal integer where an expression was
// wanted, and the failure will look like a bug in the tool.
// ---------------------------------------------------------------------------

inline Json prop(const char* type, const std::string& description) {
    return Json{{"type", type}, {"description", description}};
}

inline Json enumProp(std::vector<std::string> values, const std::string& description) {
    return Json{{"type", "string"}, {"enum", std::move(values)}, {"description", description}};
}

// An address parameter. Accepts a plain integer or any expression the address
// boxes in the UI accept -- `"client.dll+0x4A2C10"`, `"kernel32.LoadLibraryW"`,
// a user symbol -- because an expression written down once keeps working after
// the target restarts and ASLR moves the module, and a bare integer does not.
inline Json addressProp(const std::string& description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Accepts a number, or an expression such as "
                                              "\"client.dll+0x4A2C10\" or \"kernel32.LoadLibraryW\"."}};
}

inline Json objectSchema(Json properties, std::vector<std::string> required = {}) {
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)}};
}

inline Json emptySchema() {
    return Json{{"type", "object"}, {"properties", Json::object()}};
}

// ---------------------------------------------------------------------------
// Argument extraction
//
// Every one of these reports the parameter by name. A model that passed a string
// where a number was wanted can correct itself from "port must be an integer";
// it cannot from "bad argument".
// ---------------------------------------------------------------------------

inline bool has(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args.at(key).is_null();
}

inline infra::Result<std::string> requireString(const Json& args, const char* key) {
    if (!has(args, key)) {
        return infra::Result<std::string>::fail(std::string(key) + " is required.");
    }
    const auto& value = args.at(key);
    if (!value.is_string()) {
        return infra::Result<std::string>::fail(std::string(key) + " must be a string.");
    }
    return infra::Result<std::string>::ok(value.get<std::string>());
}

inline std::string optionalString(const Json& args, const char* key, std::string fallback = {}) {
    if (!has(args, key) || !args.at(key).is_string()) {
        return fallback;
    }
    return args.at(key).get<std::string>();
}

inline infra::Result<std::uint64_t> requireUint(const Json& args, const char* key) {
    if (!has(args, key)) {
        return infra::Result<std::uint64_t>::fail(std::string(key) + " is required.");
    }
    const auto& value = args.at(key);
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return infra::Result<std::uint64_t>::fail(std::string(key) + " must be an integer.");
    }
    if (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0) {
        return infra::Result<std::uint64_t>::fail(std::string(key) + " must not be negative.");
    }
    return infra::Result<std::uint64_t>::ok(value.get<std::uint64_t>());
}

inline std::uint64_t optionalUint(const Json& args, const char* key, std::uint64_t fallback) {
    if (auto value = requireUint(args, key)) {
        return value.value();
    }
    return fallback;
}

inline bool optionalBool(const Json& args, const char* key, bool fallback = false) {
    if (!has(args, key) || !args.at(key).is_boolean()) {
        return fallback;
    }
    return args.at(key).get<bool>();
}

inline double optionalDouble(const Json& args, const char* key, double fallback) {
    if (!has(args, key) || !args.at(key).is_number()) {
        return fallback;
    }
    return args.at(key).get<double>();
}

// A signed offset list, for pointer chains. A non-integer entry is an error
// rather than a silent zero: the Lua API reads one as 0 and the resulting chain
// resolves to the wrong address with nothing to say why.
inline infra::Result<std::vector<std::ptrdiff_t>> requireOffsets(const Json& args, const char* key) {
    using Offsets = std::vector<std::ptrdiff_t>;
    if (!has(args, key)) {
        return infra::Result<Offsets>::ok({});
    }
    const auto& value = args.at(key);
    if (!value.is_array()) {
        return infra::Result<Offsets>::fail(std::string(key) + " must be an array of integers.");
    }
    Offsets offsets;
    offsets.reserve(value.size());
    for (const auto& entry : value) {
        if (!entry.is_number_integer() && !entry.is_number_unsigned()) {
            return infra::Result<Offsets>::fail(std::string(key) + " must contain only integers.");
        }
        offsets.push_back(static_cast<std::ptrdiff_t>(entry.get<std::int64_t>()));
    }
    return infra::Result<Offsets>::ok(std::move(offsets));
}

// ---------------------------------------------------------------------------
// Domain conversions
// ---------------------------------------------------------------------------

inline infra::Result<domain::ValueType> requireValueType(const Json& args, const char* key,
                                                         domain::ValueType fallback = domain::ValueType::Int32) {
    if (!has(args, key)) {
        return infra::Result<domain::ValueType>::ok(fallback);
    }
    const auto text = requireString(args, key);
    if (!text) {
        return infra::Result<domain::ValueType>::fail(text.error());
    }
    const auto type = domain::parseValueType(text.value());
    if (!type) {
        return infra::Result<domain::ValueType>::fail("\"" + text.value() +
                                                      "\" is not a value type. Use i8, u8, i16, u16, i32, u32, "
                                                      "i64, u64, f32, f64, bytes, str or wstr.");
    }
    return infra::Result<domain::ValueType>::ok(*type);
}

// Scan modes arrive as their display names lowercased and hyphen-free --
// "exact", "increased by", "same as first scan" -- so the vocabulary the model
// sees is the vocabulary the UI shows. Matching is case-insensitive and ignores
// spaces, so "increasedby" works too.
inline infra::Result<domain::ScanMode> requireScanMode(const Json& args, const char* key) {
    const auto text = requireString(args, key);
    if (!text) {
        return infra::Result<domain::ScanMode>::fail(text.error());
    }
    const auto squash = [](std::string value) {
        std::string out;
        for (const char c : value) {
            if (c != ' ' && c != '_' && c != '-') {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        return out;
    };
    const auto wanted = squash(text.value());
    for (const auto mode : domain::scanModes()) {
        if (squash(domain::scanModeName(mode)) == wanted) {
            return infra::Result<domain::ScanMode>::ok(mode);
        }
    }
    // "unknown" is what the Lua API calls UnknownInitial, and it is the name
    // anyone reaching for a next-scan that matches everything will try first.
    if (wanted == "unknown") {
        return infra::Result<domain::ScanMode>::ok(domain::ScanMode::UnknownInitial);
    }
    return infra::Result<domain::ScanMode>::fail("\"" + text.value() + "\" is not a scan mode.");
}

inline std::vector<std::string> scanModeNames() {
    std::vector<std::string> names;
    for (const auto mode : domain::scanModes()) {
        names.emplace_back(domain::scanModeName(mode));
    }
    return names;
}

inline std::vector<std::string> valueTypeNames() {
    std::vector<std::string> names;
    for (const auto type : domain::valueTypes()) {
        names.emplace_back(domain::valueTypeName(type));
    }
    return names;
}

// An address argument, resolved through the symbol table so every expression the
// UI accepts works here too.
inline infra::Result<std::uintptr_t> requireAddress(services::RuntimeServices& services, const Json& args,
                                                    const char* key) {
    using Address = std::uintptr_t;
    if (!has(args, key)) {
        return infra::Result<Address>::fail(std::string(key) + " is required.");
    }
    const auto& value = args.at(key);
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return infra::Result<Address>::ok(static_cast<Address>(value.get<std::uint64_t>()));
    }
    if (!value.is_string()) {
        return infra::Result<Address>::fail(std::string(key) + " must be an address or an address expression.");
    }
    return services.symbols().resolve(services.session(), value.get<std::string>());
}

// A value argument, as text for domain::parseScanValue.
//
// Routed through the same parser the UI's boxes use rather than decoded from
// JSON's own types, so the documented leniency -- "48 8B 05", "488b05" and
// "48-8B-05" being one input -- holds here too. Floats are printed at full
// precision on the way in: a value that survived a round trip through a shorter
// form would scan for a number the caller never asked for.
inline infra::Result<std::string> requireValueText(const Json& args, const char* key) {
    if (!has(args, key)) {
        return infra::Result<std::string>::fail(std::string(key) + " is required for this scan mode.");
    }
    const auto& value = args.at(key);
    if (value.is_string()) {
        return infra::Result<std::string>::ok(value.get<std::string>());
    }
    if (value.is_number_unsigned()) {
        return infra::Result<std::string>::ok(std::to_string(value.get<std::uint64_t>()));
    }
    if (value.is_number_integer()) {
        return infra::Result<std::string>::ok(std::to_string(value.get<std::int64_t>()));
    }
    if (value.is_number_float()) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value.get<double>());
        return infra::Result<std::string>::ok(buffer);
    }
    return infra::Result<std::string>::fail(std::string(key) + " must be a number or a string.");
}

// The needle for a scan, assembled exactly as the Scanner panel assembles it:
// the second operand parsed with the same type as the first so "between 10 and
// 20" means the same thing in both halves, and a width-only placeholder for the
// modes that compare against an earlier scan and have no needle at all.
inline infra::Result<domain::ScanValue> buildScanValue(const Json& args, domain::ScanMode mode,
                                                       domain::ValueType type) {
    using Value = domain::ScanValue;
    if (!domain::modeUsesValue(mode)) {
        Value placeholder;
        placeholder.type = type;
        placeholder.bytes.assign(domain::valueTypeSize(type), 0);
        return infra::Result<Value>::ok(std::move(placeholder));
    }

    const auto text = requireValueText(args, "value");
    if (!text) {
        return infra::Result<Value>::fail(text.error());
    }
    auto parsed = domain::parseScanValue(type, text.value());
    if (!parsed) {
        return infra::Result<Value>::fail("\"" + text.value() + "\" is not a valid " +
                                          domain::valueTypeName(type) + " value.");
    }
    parsed->caseInsensitive = optionalBool(args, "case_insensitive");

    if (domain::modeUsesSecondValue(mode)) {
        const auto upperText = requireValueText(args, "value2");
        if (!upperText) {
            return infra::Result<Value>::fail(std::string(domain::scanModeName(mode)) +
                                              " needs an upper bound in value2.");
        }
        auto upper = domain::parseScanValue(type, upperText.value());
        if (!upper) {
            return infra::Result<Value>::fail("\"" + upperText.value() + "\" is not a valid " +
                                              domain::valueTypeName(type) + " value.");
        }
        parsed->bytes2 = std::move(upper->bytes);
        parsed->text2 = upperText.value();
    }
    return infra::Result<Value>::ok(std::move(*parsed));
}

// ---------------------------------------------------------------------------
// Result shaping
//
// Addresses go out as both a number and a hex string. The number is what a
// caller passes back to the next tool; the hex is what it should show a person,
// and asking a model to format one is asking it to get it wrong occasionally.
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] inline std::optional<T> readRaw(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < sizeof(T)) {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

// Bytes decoded into a JSON value of the natural type, so a caller can compare
// numbers without parsing text. Null when the buffer is too short for the type,
// which is how a short read surfaces.
inline Json decodeValue(domain::ValueType type, const std::vector<std::uint8_t>& bytes) {
    switch (type) {
    case domain::ValueType::Int8:
        if (auto v = readRaw<std::int8_t>(bytes)) { return Json(static_cast<std::int64_t>(*v)); }
        break;
    case domain::ValueType::UInt8:
        if (auto v = readRaw<std::uint8_t>(bytes)) { return Json(static_cast<std::uint64_t>(*v)); }
        break;
    case domain::ValueType::Int16:
        if (auto v = readRaw<std::int16_t>(bytes)) { return Json(static_cast<std::int64_t>(*v)); }
        break;
    case domain::ValueType::UInt16:
        if (auto v = readRaw<std::uint16_t>(bytes)) { return Json(static_cast<std::uint64_t>(*v)); }
        break;
    case domain::ValueType::Int32:
        if (auto v = readRaw<std::int32_t>(bytes)) { return Json(static_cast<std::int64_t>(*v)); }
        break;
    case domain::ValueType::UInt32:
        if (auto v = readRaw<std::uint32_t>(bytes)) { return Json(static_cast<std::uint64_t>(*v)); }
        break;
    case domain::ValueType::Int64:
        if (auto v = readRaw<std::int64_t>(bytes)) { return Json(*v); }
        break;
    case domain::ValueType::UInt64:
        if (auto v = readRaw<std::uint64_t>(bytes)) { return Json(*v); }
        break;
    case domain::ValueType::Float:
        if (auto v = readRaw<float>(bytes)) { return Json(static_cast<double>(*v)); }
        break;
    case domain::ValueType::Double:
        if (auto v = readRaw<double>(bytes)) { return Json(*v); }
        break;
    case domain::ValueType::Bytes:
        return Json(domain::bytesToHex(bytes, false));
    case domain::ValueType::StringAscii:
    case domain::ValueType::StringUtf16:
        return Json(domain::formatValue(type, bytes));
    }
    return Json(nullptr);
}

inline Json addressJson(std::uintptr_t address) {
    return Json{{"address", static_cast<std::uint64_t>(address)}, {"hex", domain::toHex(address)}};
}

inline std::string narrow(const std::wstring& text) {
    return domain::narrow(text);
}

} // namespace ire::mcp
