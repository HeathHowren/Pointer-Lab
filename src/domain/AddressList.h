#pragma once

#include "domain/Domain.h"

#include <mutex>
#include <vector>

namespace ire::domain {

// The tracked addresses, shared between the UI thread and the freeze loop.
//
// Every accessor takes the lock, and reads hand back a copy rather than a
// reference: the freeze loop writes to this list while the UI is drawing from
// it, so a caller that held a reference would be iterating a vector another
// thread can reallocate.
class AddressList {
public:
    // Assigns and returns a fresh id; the id in the argument is ignored.
    std::uint64_t add(AddressEntry entry);
    bool remove(std::uint64_t id);
    // Replaces the entry with the same id. Returns false if there is no such id.
    bool update(const AddressEntry& entry);
    bool setFrozen(std::uint64_t id, bool frozen);
    [[nodiscard]] std::vector<AddressEntry> snapshot() const;
    // Used when loading a project. Ids in the loaded entries are preserved, and
    // the next generated id continues past the highest of them.
    void replace(std::vector<AddressEntry> entries);

private:
    mutable std::mutex mutex_;
    std::uint64_t nextId_{1};
    std::vector<AddressEntry> entries_;
};

} // namespace ire::domain

