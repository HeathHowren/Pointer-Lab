#include "engine_debug/AccessWatch.h"

#include <algorithm>

namespace ire::engine_debug {

AccessWatch::AccessWatch(domain::TargetSession& session, const engine_disasm::Disassembler& disassembler)
    : session_(session), disassembler_(disassembler) {}

void AccessWatch::begin(std::uintptr_t address, std::uint8_t length, domain::BreakpointKind kind) {
    std::scoped_lock lock(mutex_);
    sites_.clear();
    totalHits_ = 0;
    truncated_ = false;
    watched_ = address;
    length_ = length;
    kind_ = kind;
    active_ = true;
}

void AccessWatch::stop() {
    std::scoped_lock lock(mutex_);
    active_ = false;
}

bool AccessWatch::active() const {
    std::scoped_lock lock(mutex_);
    return active_;
}

std::uintptr_t AccessWatch::watchedAddress() const {
    std::scoped_lock lock(mutex_);
    return watched_;
}

std::uint8_t AccessWatch::watchedLength() const {
    std::scoped_lock lock(mutex_);
    return length_;
}

domain::BreakpointKind AccessWatch::kind() const {
    std::scoped_lock lock(mutex_);
    return kind_;
}

void AccessWatch::record(const domain::RegisterContext& context) {
    const auto trap = static_cast<std::uintptr_t>(context.rip);

    {
        std::scoped_lock lock(mutex_);
        if (!active_) {
            return;
        }

        // The hot path: an instruction already seen. No target reads, no
        // disassembly -- just a counter and the newest register state, which is
        // what makes a breakpoint in a render loop affordable.
        if (const auto existing = sites_.find(trap); existing != sites_.end()) {
            existing->second.hitCount += 1;
            existing->second.lastContext = context;
            totalHits_ += 1;
            return;
        }

        if (sites_.size() >= maxSites) {
            truncated_ = true;
            totalHits_ += 1;
            return;
        }
    }

    // Resolving happens outside the lock. It reads and disassembles target
    // memory, and holding the mutex across that would block the UI thread
    // polling sites() for the duration of several ReadProcessMemory calls.
    AccessSite site;
    site.trapAddress = trap;
    site.hitCount = 1;
    site.lastContext = context;

    // An execute breakpoint faults *before* its instruction, so the reported
    // address is already the right one. A data breakpoint traps *after* the
    // access, so the instruction that did it is the one ending here.
    const bool dataWatch = kind() != domain::BreakpointKind::HardwareExecute;
    if (dataWatch) {
        if (auto found = engine_disasm::precedingInstruction(disassembler_, session_, trap)) {
            site.address = found->address;
            site.text = found->text;
            site.bytes = found->bytes;
            site.instructionResolved = true;
        }
    } else {
        const auto listing = disassembler_.disassemble(session_, trap, 1);
        if (!listing.empty()) {
            site.address = listing.front().address;
            site.text = listing.front().text;
            site.bytes = listing.front().bytes;
            site.instructionResolved = true;
        }
    }

    if (!site.instructionResolved) {
        // Say what is known rather than guessing. `address` falls back to the
        // trap address so the UI still has somewhere to navigate to.
        site.address = trap;
        site.text = dataWatch ? "(could not identify the accessing instruction)"
                              : "(could not decode this instruction)";
    }

    std::scoped_lock lock(mutex_);
    if (!active_) {
        return;
    }
    // Another thread may have inserted the same trap address while this one was
    // disassembling, in which case the counts must merge rather than one
    // overwriting the other.
    if (const auto existing = sites_.find(trap); existing != sites_.end()) {
        existing->second.hitCount += 1;
        existing->second.lastContext = context;
    } else {
        sites_.emplace(trap, std::move(site));
    }
    totalHits_ += 1;
}

std::vector<AccessSite> AccessWatch::sites() const {
    std::scoped_lock lock(mutex_);
    std::vector<AccessSite> result;
    result.reserve(sites_.size());
    for (const auto& [trap, site] : sites_) {
        result.push_back(site);
    }
    // Busiest first: the instruction responsible for a value is almost always
    // the one hitting it most, and the reader should not have to hunt for it.
    std::sort(result.begin(), result.end(), [](const AccessSite& a, const AccessSite& b) {
        if (a.hitCount != b.hitCount) {
            return a.hitCount > b.hitCount;
        }
        return a.address < b.address;
    });
    return result;
}

std::uint64_t AccessWatch::totalHits() const {
    std::scoped_lock lock(mutex_);
    return totalHits_;
}

bool AccessWatch::truncated() const {
    std::scoped_lock lock(mutex_);
    return truncated_;
}

void AccessWatch::clear() {
    std::scoped_lock lock(mutex_);
    sites_.clear();
    totalHits_ = 0;
    truncated_ = false;
}

std::vector<RegisterMeaning> AccessWatch::explain(const domain::RegisterContext& context) const {
    const auto watched = watchedAddress();
    const auto modules = session_.modules();
    const auto regions = session_.regions();

    std::vector<RegisterMeaning> result;
    const auto count = domain::registerCount(context.bitness);
    result.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        RegisterMeaning meaning;
        meaning.name = domain::registerName(context.bitness, i);
        meaning.value = domain::registerValue(context, i);
        const auto value = static_cast<std::uintptr_t>(meaning.value);

        if (value == 0) {
            meaning.interpretation = "null";
            result.push_back(std::move(meaning));
            continue;
        }

        // The one that matters most. If the watched address sits a short way
        // above this register, the register is very likely the base of the
        // structure the value belongs to -- which is the answer to "what object
        // is my health actually part of?".
        if (watched >= value && watched - value <= maxStructOffset) {
            const auto offset = watched - value;
            meaning.interpretation = offset == 0
                                         ? "the watched address itself"
                                         : "watched address is " + meaning.name + "+" + domain::toHex(offset) +
                                               " -- likely the structure base";
            result.push_back(std::move(meaning));
            continue;
        }

        // Inside a loaded module: a static address, and worth naming as one.
        const auto module = std::find_if(modules.begin(), modules.end(), [value](const domain::ModuleInfo& m) {
            return value >= m.base && value < m.base + m.size;
        });
        if (module != modules.end()) {
            meaning.interpretation =
                domain::narrow(module->name) + "+" + domain::toHex(value - module->base) + " (static)";
            result.push_back(std::move(meaning));
            continue;
        }

        const auto region = std::find_if(regions.begin(), regions.end(), [value](const domain::MemoryRegion& r) {
            return value >= r.base && value < r.base + r.size;
        });
        if (region != regions.end()) {
            std::string description = "points into ";
            description += region->executable ? "executable " : (region->writable ? "writable " : "readable ");
            description += "memory at " + domain::toHex(region->base);
            meaning.interpretation = std::move(description);
            result.push_back(std::move(meaning));
            continue;
        }

        // Not a pointer into anything mapped, so almost certainly a plain
        // number. Saying nothing is better than inventing a meaning for it.
        result.push_back(std::move(meaning));
    }

    return result;
}

} // namespace ire::engine_debug
