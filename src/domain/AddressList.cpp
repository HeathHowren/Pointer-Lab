#include "domain/AddressList.h"

#include <algorithm>

namespace ire::domain {

std::uint64_t AddressList::add(AddressEntry entry) {
    std::scoped_lock lock(mutex_);
    entry.id = nextId_++;
    entries_.push_back(entry);
    return entry.id;
}

bool AddressList::remove(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const AddressEntry& e) { return e.id == id; }), entries_.end());
    return entries_.size() != before;
}

bool AddressList::update(const AddressEntry& entry) {
    std::scoped_lock lock(mutex_);
    for (auto& existing : entries_) {
        if (existing.id == entry.id) {
            existing = entry;
            return true;
        }
    }
    return false;
}

bool AddressList::setFrozen(std::uint64_t id, bool frozen) {
    std::scoped_lock lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.id == id) {
            entry.frozen = frozen;
            return true;
        }
    }
    return false;
}

std::vector<AddressEntry> AddressList::snapshot() const {
    std::scoped_lock lock(mutex_);
    return entries_;
}

void AddressList::replace(std::vector<AddressEntry> entries) {
    std::scoped_lock lock(mutex_);
    entries_ = std::move(entries);
    nextId_ = 1;
    for (const auto& entry : entries_) {
        nextId_ = std::max(nextId_, entry.id + 1);
    }
}

} // namespace ire::domain

