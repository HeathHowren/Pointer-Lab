#include "domain/Structure.h"

#include <algorithm>
#include <sstream>

namespace ire::domain {

std::size_t Structure::sizeInBytes() const {
    std::ptrdiff_t furthest = 0;
    for (const auto& field : fields) {
        furthest = std::max(furthest, field.end());
    }
    return furthest > 0 ? static_cast<std::size_t>(furthest) : 0;
}

const StructureField* Structure::fieldAt(std::ptrdiff_t offset) const {
    const auto found = std::find_if(fields.begin(), fields.end(),
                                    [offset](const StructureField& f) { return f.offset == offset; });
    return found == fields.end() ? nullptr : &*found;
}

const StructureField* Structure::fieldCovering(std::ptrdiff_t offset) const {
    const auto found = std::find_if(fields.begin(), fields.end(), [offset](const StructureField& f) {
        return offset >= f.offset && offset < f.end();
    });
    return found == fields.end() ? nullptr : &*found;
}

std::string defaultFieldName(std::ptrdiff_t offset) {
    std::ostringstream out;
    // Negative offsets are legal: a structure's start is a guess, and finding
    // that the real object begins earlier should not mean renumbering
    // everything below it.
    if (offset < 0) {
        out << "field_minus_" << std::hex << static_cast<std::uintptr_t>(-offset);
    } else {
        out << "field_" << std::hex << static_cast<std::uintptr_t>(offset);
    }
    return out.str();
}

} // namespace ire::domain
