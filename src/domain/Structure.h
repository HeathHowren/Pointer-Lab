#pragma once

#include "domain/Domain.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ire::domain {

// One named slot at a fixed offset from the start of a structure.
//
// `length` exists only for the types whose width does not come from the type
// itself -- Bytes and the two string types. For everything else it is ignored
// and size() answers from the type, so a field can never claim a width its type
// cannot have.
struct StructureField {
    std::ptrdiff_t offset{};
    ValueType type{ValueType::Int32};
    std::size_t length{};
    std::string name;

    [[nodiscard]] std::size_t size() const {
        const auto fixed = valueTypeSize(type);
        return fixed != 0 ? fixed : length;
    }
    [[nodiscard]] std::ptrdiff_t end() const {
        return offset + static_cast<std::ptrdiff_t>(size());
    }
};

// A layout: what lives where, relative to the start of an object.
//
// The reason to write one down is that a game does not have one player, it has
// an array of them. Once you know that health is at +0x9C of *something*, the
// same +0x9C answers the same question for every enemy on the map -- and that
// is the step from a cheat that works this session to an understanding that
// keeps working.
//
// Fields are kept sorted by offset and may not overlap. Overlapping fields are
// refused rather than tolerated because the display would have to pick one, and
// whichever it picked would be wrong half the time.
struct Structure {
    std::uint64_t id{};
    std::string name;
    std::vector<StructureField> fields;

    // Distance from the start to the end of the furthest field, which is how
    // many bytes have to be read to fill the whole layout in.
    [[nodiscard]] std::size_t sizeInBytes() const;
    [[nodiscard]] const StructureField* fieldAt(std::ptrdiff_t offset) const;
    // The field whose range contains this offset, which is not the same
    // question: an 8-byte field at +0x10 covers +0x14.
    [[nodiscard]] const StructureField* fieldCovering(std::ptrdiff_t offset) const;
};

// A name for a field nobody has named yet. Deliberately the offset rather than
// "field1", so a name carries information even before anyone has worked out
// what the field is for, and renumbering never happens when a field is
// inserted above it.
[[nodiscard]] std::string defaultFieldName(std::ptrdiff_t offset);

} // namespace ire::domain
