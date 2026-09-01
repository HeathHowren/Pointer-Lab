#pragma once

#include "domain/Structure.h"
#include "domain/TargetSession.h"
#include "engine_symbols/SymbolTable.h"
#include "infra/Result.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ire::engine_struct {

// One field read from one address.
struct Cell {
    // False when that address could not be read at all -- the object has been
    // freed, or the guess about where it starts was wrong. Shown as such rather
    // than as zeroes, which would look like a value.
    bool read{};
    std::vector<std::uint8_t> bytes;
    std::string text;
    // What the value appears to be, when that is worth saying: where a pointer
    // points, whether it lands in a module. Empty when there is nothing to add.
    std::string annotation;
};

struct Row {
    domain::StructureField field;
    // One per address, in the order they were given.
    std::vector<Cell> cells;
    // True when every address that could be read holds the same bytes here.
    // This is the whole reason for putting instances side by side: the fields
    // that differ between two players are the ones that describe a player, and
    // the fields that do not are shared state or padding.
    bool identical{};
};

struct Snapshot {
    std::vector<std::uintptr_t> addresses;
    std::vector<Row> rows;
    // Addresses that could not be read at all, by index into `addresses`.
    std::vector<std::size_t> unreadable;
};

// Applies a named layout to several addresses at once.
//
// A hex editor answers "what bytes are here". This answers "what is this
// object, and which parts of it are different in that other one" -- which is
// the question that turns one found value into a structure, and a structure
// into every other instance of the same thing.
class Dissector {
public:
    Dissector(domain::TargetSession& session, const engine_symbols::SymbolTable& symbols);

    std::uint64_t add(std::string name);
    infra::Result<void> remove(std::uint64_t id);
    infra::Result<void> rename(std::uint64_t id, std::string name);
    // Adds a field, or replaces the one that already starts at that offset.
    // Refuses when the new field would overlap a different one: the display
    // would have to pick which of the two owns the byte, and whichever it
    // picked would be wrong half the time.
    infra::Result<void> setField(std::uint64_t id, domain::StructureField field);
    infra::Result<void> removeField(std::uint64_t id, std::ptrdiff_t offset);
    // Replaces the whole field list at once, for loading a saved definition.
    infra::Result<void> setFields(std::uint64_t id, std::vector<domain::StructureField> fields);
    void forgetAll();

    [[nodiscard]] std::vector<domain::Structure> structures() const;
    [[nodiscard]] std::optional<domain::Structure> find(std::uint64_t id) const;

    // Reads each address once and lays the structure over what came back. One
    // read per address rather than one per field: a structure with forty fields
    // over four instances is 160 round trips into another process otherwise,
    // and the panel does this every frame.
    [[nodiscard]] infra::Result<Snapshot> read(std::uint64_t id,
                                               const std::vector<std::uintptr_t>& addresses) const;

    // Fills a structure in from what is actually at those addresses: one field
    // per slot, typed by what the bytes look like. Replaces any existing
    // fields. Returns how many were created.
    //
    // This is a guess and is labelled as one everywhere it appears. It is still
    // worth having: naming forty slots by hand before you know which of them
    // matter is how people give up on structures.
    infra::Result<std::size_t> guess(std::uint64_t id, const std::vector<std::uintptr_t>& addresses,
                                     std::size_t size);

    // The classification on its own, given one slot's bytes as seen at each
    // address. Exposed because it is the part worth testing directly, and
    // because the UI offers it for a single field.
    [[nodiscard]] domain::ValueType classify(const std::vector<std::vector<std::uint8_t>>& samples) const;

    // How much of an object to read when nobody says. Four pages: enough to
    // cover any player-sized object, small enough that reading it every frame
    // for four instances costs nothing.
    static constexpr std::size_t defaultSize = 0x400;
    // Refused above this, because a structure panel showing sixteen thousand
    // rows is not showing anything.
    static constexpr std::size_t maxSize = 0x4000;
    // More columns than this stop being comparable and start being a spreadsheet.
    static constexpr std::size_t maxAddresses = 8;

private:
    // True when this slot holds something that could be a pointer at every
    // address: null, or an address inside a committed region. Requires at least
    // one non-null, or every run of zeroes in the object would be a pointer.
    [[nodiscard]] bool looksLikePointer(const std::vector<std::vector<std::uint8_t>>& samples) const;

    domain::TargetSession& session_;
    const engine_symbols::SymbolTable& symbols_;

    mutable std::mutex mutex_;
    std::vector<domain::Structure> structures_;
    std::uint64_t nextId_{1};

    // The sorted committed-region ranges the pointer heuristic searches, kept
    // between frames. Building it copies and sorts every region in the target
    // -- tens of thousands of them -- and read() runs every frame the panel is
    // open. The generation it was built at is the only thing that can
    // invalidate it, because only attach/refresh/detach change the regions.
    struct RegionCache;
    // Handed out by value so a caller keeps the index alive even if another
    // thread swaps in a rebuilt one behind it.
    [[nodiscard]] std::shared_ptr<const RegionCache> cachedRegions() const;
    mutable std::mutex regionMutex_;
    mutable std::shared_ptr<const RegionCache> regionCache_;
};

// True when these four or eight bytes read as a number a person would plausibly
// have stored as a float or a double: finite, not denormal, and within the range
// real quantities live in. Free functions because they are pure, and because the
// tests for them should not need a target process.
//
// The heuristic works because the two interpretations barely overlap. A small
// integer -- 100, 3, 65535 -- reads as a denormal float around 1e-43, and a
// float in the ordinary range reads as an integer in the hundreds of millions.
// Neither is the kind of number the other kind of field usually holds.
[[nodiscard]] bool looksLikeFloat(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] bool looksLikeDouble(const std::vector<std::uint8_t>& bytes);

} // namespace ire::engine_struct
