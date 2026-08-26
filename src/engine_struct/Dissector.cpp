#include "engine_struct/Dissector.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ire::engine_struct {

namespace {

template <typename T>
using Result = infra::Result<T>;

bool allZero(const std::vector<std::uint8_t>& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
}

std::uint64_t toLittleEndian(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size() && i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

// Committed, readable ranges, sorted, so "could this number be a pointer?" is a
// binary search rather than a walk over every region in the process. The guess
// pass asks it once per four bytes of the object.
class RegionIndex {
public:
    explicit RegionIndex(const domain::TargetSession& session) {
        for (const auto& region : session.regions()) {
            if (region.state == MEM_COMMIT && region.readable) {
                ranges_.emplace_back(region.base, region.base + region.size);
            }
        }
        std::sort(ranges_.begin(), ranges_.end());
    }

    [[nodiscard]] bool contains(std::uintptr_t address) const {
        // Nothing legitimate lives in the first 64 KB; Windows reserves it
        // precisely so a null-ish pointer faults instead of reading something.
        if (address < 0x10000) {
            return false;
        }
        const auto after = std::upper_bound(ranges_.begin(), ranges_.end(), address,
                                            [](std::uintptr_t value, const auto& range) {
                                                return value < range.first;
                                            });
        if (after == ranges_.begin()) {
            return false;
        }
        const auto& range = *std::prev(after);
        return address >= range.first && address < range.second;
    }

private:
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges_;
};

bool pointerLike(const std::vector<std::vector<std::uint8_t>>& samples, std::size_t pointerSize,
                 const RegionIndex& regions) {
    if (samples.empty()) {
        return false;
    }
    bool anyReal = false;
    for (const auto& sample : samples) {
        if (sample.size() != pointerSize) {
            return false;
        }
        const auto value = static_cast<std::uintptr_t>(toLittleEndian(sample));
        if (value == 0) {
            // A null pointer in one instance out of four is ordinary -- the
            // slot is still a pointer. What is not allowed is *every* instance
            // being null, or every run of zeroes in the object would qualify.
            continue;
        }
        if (!regions.contains(value)) {
            return false;
        }
        anyReal = true;
    }
    return anyReal;
}

template <typename Predicate>
bool everySampleOrZero(const std::vector<std::vector<std::uint8_t>>& samples, Predicate&& predicate) {
    if (samples.empty()) {
        return false;
    }
    bool anyReal = false;
    for (const auto& sample : samples) {
        if (allZero(sample)) {
            continue;
        }
        if (!predicate(sample)) {
            return false;
        }
        anyReal = true;
    }
    return anyReal;
}

} // namespace

bool looksLikeFloat(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 4) {
        return false;
    }
    float value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    if (!std::isfinite(value) || value == 0.0f) {
        return false;
    }
    const float magnitude = std::fabs(value);
    // Positions, healths, speeds and timers all live inside this. Outside it
    // are the denormals a small integer decodes to, and the astronomical values
    // a large one does.
    return magnitude >= 1e-6f && magnitude <= 1e9f;
}

bool looksLikeDouble(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 8) {
        return false;
    }
    double value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    if (!std::isfinite(value) || value == 0.0) {
        return false;
    }
    const double magnitude = std::fabs(value);
    return magnitude >= 1e-6 && magnitude <= 1e9;
}

Dissector::Dissector(domain::TargetSession& session, const engine_symbols::SymbolTable& symbols)
    : session_(session), symbols_(symbols) {}

std::uint64_t Dissector::add(std::string name) {
    std::scoped_lock lock(mutex_);
    domain::Structure structure;
    structure.id = nextId_++;
    structure.name = name.empty() ? "Structure " + std::to_string(structure.id) : std::move(name);
    structures_.push_back(structure);
    return structure.id;
}

infra::Result<void> Dissector::remove(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto removed = std::remove_if(structures_.begin(), structures_.end(),
                                        [id](const domain::Structure& s) { return s.id == id; });
    if (removed == structures_.end()) {
        return Result<void>::fail("That structure is no longer in the list.");
    }
    structures_.erase(removed, structures_.end());
    return Result<void>::ok();
}

infra::Result<void> Dissector::rename(std::uint64_t id, std::string name) {
    std::scoped_lock lock(mutex_);
    const auto found = std::find_if(structures_.begin(), structures_.end(),
                                    [id](const domain::Structure& s) { return s.id == id; });
    if (found == structures_.end()) {
        return Result<void>::fail("That structure is no longer in the list.");
    }
    if (name.empty()) {
        return Result<void>::fail("A structure needs a name.");
    }
    found->name = std::move(name);
    return Result<void>::ok();
}

infra::Result<void> Dissector::setField(std::uint64_t id, domain::StructureField field) {
    std::scoped_lock lock(mutex_);
    const auto structure = std::find_if(structures_.begin(), structures_.end(),
                                        [id](const domain::Structure& s) { return s.id == id; });
    if (structure == structures_.end()) {
        return Result<void>::fail("That structure is no longer in the list.");
    }
    if (field.size() == 0) {
        return Result<void>::fail("A " + std::string(domain::valueTypeName(field.type)) +
                                  " field needs a length, because its width does not come from its type.");
    }
    for (const auto& existing : structure->fields) {
        if (existing.offset == field.offset) {
            continue; // this one is being replaced
        }
        if (field.offset < existing.end() && existing.offset < field.end()) {
            return Result<void>::fail("That field would overlap \"" + existing.name + "\" at +" +
                                      domain::toHex(static_cast<std::uintptr_t>(existing.offset)) +
                                      ". Two fields cannot own the same byte: the display would have to "
                                      "pick one, and it would be wrong half the time.");
        }
    }
    if (field.name.empty()) {
        field.name = domain::defaultFieldName(field.offset);
    }

    const auto same = std::find_if(structure->fields.begin(), structure->fields.end(),
                                   [&field](const domain::StructureField& f) {
                                       return f.offset == field.offset;
                                   });
    if (same != structure->fields.end()) {
        *same = std::move(field);
    } else {
        structure->fields.push_back(std::move(field));
        std::sort(structure->fields.begin(), structure->fields.end(),
                  [](const domain::StructureField& a, const domain::StructureField& b) {
                      return a.offset < b.offset;
                  });
    }
    return Result<void>::ok();
}

infra::Result<void> Dissector::removeField(std::uint64_t id, std::ptrdiff_t offset) {
    std::scoped_lock lock(mutex_);
    const auto structure = std::find_if(structures_.begin(), structures_.end(),
                                        [id](const domain::Structure& s) { return s.id == id; });
    if (structure == structures_.end()) {
        return Result<void>::fail("That structure is no longer in the list.");
    }
    const auto removed = std::remove_if(structure->fields.begin(), structure->fields.end(),
                                        [offset](const domain::StructureField& f) {
                                            return f.offset == offset;
                                        });
    if (removed == structure->fields.end()) {
        return Result<void>::fail("There is no field at that offset.");
    }
    structure->fields.erase(removed, structure->fields.end());
    return Result<void>::ok();
}

infra::Result<void> Dissector::setFields(std::uint64_t id, std::vector<domain::StructureField> fields) {
    std::sort(fields.begin(), fields.end(),
              [](const domain::StructureField& a, const domain::StructureField& b) {
                  return a.offset < b.offset;
              });
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].size() == 0) {
            return Result<void>::fail("Field \"" + fields[i].name + "\" has no width.");
        }
        if (i > 0 && fields[i].offset < fields[i - 1].end()) {
            return Result<void>::fail("Fields \"" + fields[i - 1].name + "\" and \"" + fields[i].name +
                                      "\" overlap.");
        }
        if (fields[i].name.empty()) {
            fields[i].name = domain::defaultFieldName(fields[i].offset);
        }
    }

    std::scoped_lock lock(mutex_);
    const auto structure = std::find_if(structures_.begin(), structures_.end(),
                                        [id](const domain::Structure& s) { return s.id == id; });
    if (structure == structures_.end()) {
        return Result<void>::fail("That structure is no longer in the list.");
    }
    structure->fields = std::move(fields);
    return Result<void>::ok();
}

void Dissector::forgetAll() {
    std::scoped_lock lock(mutex_);
    structures_.clear();
}

std::vector<domain::Structure> Dissector::structures() const {
    std::scoped_lock lock(mutex_);
    return structures_;
}

std::optional<domain::Structure> Dissector::find(std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    const auto found = std::find_if(structures_.begin(), structures_.end(),
                                    [id](const domain::Structure& s) { return s.id == id; });
    if (found == structures_.end()) {
        return std::nullopt;
    }
    return *found;
}

bool Dissector::looksLikePointer(const std::vector<std::vector<std::uint8_t>>& samples) const {
    const RegionIndex regions(session_);
    return pointerLike(samples, session_.pointerSize(), regions);
}

domain::ValueType Dissector::classify(const std::vector<std::vector<std::uint8_t>>& samples) const {
    if (samples.empty()) {
        return domain::ValueType::Bytes;
    }
    const auto width = samples.front().size();
    if (std::any_of(samples.begin(), samples.end(),
                    [width](const std::vector<std::uint8_t>& s) { return s.size() != width; })) {
        return domain::ValueType::Bytes;
    }

    const RegionIndex regions(session_);
    const auto pointerWidth = session_.pointerSize();

    if (width == pointerWidth && pointerLike(samples, pointerWidth, regions)) {
        // Deliberately unsigned: an address printed as a negative number is
        // technically the same bits and useless to read.
        return pointerWidth == 8 ? domain::ValueType::UInt64 : domain::ValueType::UInt32;
    }
    if (width == 8) {
        return everySampleOrZero(samples, looksLikeDouble) ? domain::ValueType::Double
                                                           : domain::ValueType::Int64;
    }
    if (width == 4) {
        return everySampleOrZero(samples, looksLikeFloat) ? domain::ValueType::Float
                                                          : domain::ValueType::Int32;
    }
    return domain::ValueType::Bytes;
}

infra::Result<Snapshot> Dissector::read(std::uint64_t id,
                                        const std::vector<std::uintptr_t>& addresses) const {
    const auto structure = find(id);
    if (!structure) {
        return Result<Snapshot>::fail("That structure is no longer in the list.");
    }
    if (addresses.empty()) {
        return Result<Snapshot>::fail("Give at least one address to lay the structure over.");
    }
    if (addresses.size() > maxAddresses) {
        return Result<Snapshot>::fail("At most " + std::to_string(maxAddresses) +
                                      " addresses can be compared side by side.");
    }
    if (!session_.attached()) {
        return Result<Snapshot>::fail("Attach to a process first.");
    }

    Snapshot snapshot;
    snapshot.addresses = addresses;
    if (structure->fields.empty()) {
        return Result<Snapshot>::ok(std::move(snapshot));
    }

    // A field may sit at a negative offset: the start of an object is a guess,
    // and discovering the real one begins earlier should not mean renumbering
    // everything below it. So the window read is from the lowest offset used,
    // not from zero.
    std::ptrdiff_t lowest = 0;
    std::ptrdiff_t highest = 0;
    for (const auto& field : structure->fields) {
        lowest = std::min(lowest, field.offset);
        highest = std::max(highest, field.end());
    }
    const auto span = static_cast<std::size_t>(highest - lowest);

    // One read per address rather than one per field. A forty-field structure
    // over four instances is 160 cross-process round trips the other way, and
    // this runs every frame.
    std::vector<std::vector<std::uint8_t>> windows(addresses.size());
    std::vector<bool> readable(addresses.size(), false);
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        auto bytes = session_.readBytes(addresses[i] + static_cast<std::uintptr_t>(lowest), span);
        if (bytes && bytes.value().size() == span) {
            windows[i] = std::move(bytes.value());
            readable[i] = true;
        } else {
            snapshot.unreadable.push_back(i);
        }
    }

    const RegionIndex regions(session_);
    const auto pointerWidth = session_.pointerSize();

    for (const auto& field : structure->fields) {
        Row row;
        row.field = field;
        row.cells.resize(addresses.size());

        const auto start = static_cast<std::size_t>(field.offset - lowest);
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            if (!readable[i]) {
                continue;
            }
            auto& cell = row.cells[i];
            cell.read = true;
            cell.bytes.assign(windows[i].begin() + static_cast<std::ptrdiff_t>(start),
                              windows[i].begin() + static_cast<std::ptrdiff_t>(start + field.size()));
            cell.text = domain::formatValue(field.type, cell.bytes);

            // Only for a slot the size of a pointer, and only for the unsigned
            // types. Annotating every int32 that happens to fall in a mapped
            // range would bury the ones that mean something.
            const bool pointerShaped = cell.bytes.size() == pointerWidth &&
                                       (field.type == domain::ValueType::UInt64 ||
                                        field.type == domain::ValueType::UInt32);
            if (pointerShaped) {
                const auto value = static_cast<std::uintptr_t>(toLittleEndian(cell.bytes));
                if (value != 0 && regions.contains(value)) {
                    const auto described = symbols_.describe(session_, value);
                    cell.annotation = "-> " + (described.empty() ? domain::toHex(value) : described);
                }
            }
        }

        // Compared across the addresses that could be read. An instance that
        // has gone away must not make the remaining ones look like they differ.
        std::vector<std::uint8_t> reference;
        bool first = true;
        row.identical = true;
        for (const auto& cell : row.cells) {
            if (!cell.read) {
                continue;
            }
            if (first) {
                reference = cell.bytes;
                first = false;
            } else if (cell.bytes != reference) {
                row.identical = false;
                break;
            }
        }

        snapshot.rows.push_back(std::move(row));
    }

    return Result<Snapshot>::ok(std::move(snapshot));
}

infra::Result<std::size_t> Dissector::guess(std::uint64_t id, const std::vector<std::uintptr_t>& addresses,
                                            std::size_t size) {
    if (!find(id)) {
        return Result<std::size_t>::fail("That structure is no longer in the list.");
    }
    if (addresses.empty()) {
        return Result<std::size_t>::fail("Give at least one address to read.");
    }
    if (!session_.attached()) {
        return Result<std::size_t>::fail("Attach to a process first.");
    }
    if (size == 0) {
        size = defaultSize;
    }
    if (size > maxSize) {
        return Result<std::size_t>::fail("At most " + domain::toHex(maxSize) +
                                         " bytes: a structure of more rows than that is not readable.");
    }
    size -= size % 4;
    if (size < 4) {
        return Result<std::size_t>::fail("Read at least four bytes.");
    }

    std::vector<std::vector<std::uint8_t>> windows;
    for (const auto address : addresses) {
        auto bytes = session_.readBytes(address, size);
        if (bytes && bytes.value().size() == size) {
            windows.push_back(std::move(bytes.value()));
        }
    }
    if (windows.empty()) {
        return Result<std::size_t>::fail(
            "None of those addresses could be read for " + std::to_string(size) +
            " bytes. Either the object is smaller than that, or it does not start where you think.");
    }

    const RegionIndex regions(session_);
    const auto pointerWidth = session_.pointerSize();
    const auto slice = [&windows](std::size_t offset, std::size_t width) {
        std::vector<std::vector<std::uint8_t>> samples;
        for (const auto& window : windows) {
            samples.emplace_back(window.begin() + static_cast<std::ptrdiff_t>(offset),
                                 window.begin() + static_cast<std::ptrdiff_t>(offset + width));
        }
        return samples;
    };

    std::vector<domain::StructureField> fields;
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        // Eight-byte slots are only considered where an eight-byte value could
        // actually be: a compiler does not put a pointer or a double at an
        // offset it is not aligned for, and looking anyway finds spurious
        // "pointers" straddling two unrelated fields.
        if (offset % 8 == 0 && offset + 8 <= size) {
            const auto samples = slice(offset, 8);
            if (pointerWidth == 8 && pointerLike(samples, 8, regions)) {
                fields.push_back({static_cast<std::ptrdiff_t>(offset), domain::ValueType::UInt64, 0,
                                  domain::defaultFieldName(static_cast<std::ptrdiff_t>(offset))});
                offset += 8;
                continue;
            }
            if (everySampleOrZero(samples, looksLikeDouble)) {
                fields.push_back({static_cast<std::ptrdiff_t>(offset), domain::ValueType::Double, 0,
                                  domain::defaultFieldName(static_cast<std::ptrdiff_t>(offset))});
                offset += 8;
                continue;
            }
        }

        const auto samples = slice(offset, 4);
        auto type = domain::ValueType::Int32;
        if (pointerWidth == 4 && pointerLike(samples, 4, regions)) {
            type = domain::ValueType::UInt32;
        } else if (everySampleOrZero(samples, looksLikeFloat)) {
            type = domain::ValueType::Float;
        }
        fields.push_back({static_cast<std::ptrdiff_t>(offset), type, 0,
                          domain::defaultFieldName(static_cast<std::ptrdiff_t>(offset))});
        offset += 4;
    }

    const auto count = fields.size();
    if (auto applied = setFields(id, std::move(fields)); !applied) {
        return Result<std::size_t>::fail(applied.error());
    }
    return Result<std::size_t>::ok(count);
}

} // namespace ire::engine_struct
