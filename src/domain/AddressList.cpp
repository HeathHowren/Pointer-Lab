#include "domain/AddressList.h"

#include <algorithm>

namespace ire::domain {

std::uint64_t AddressList::add(AddressEntry entry) {
    std::scoped_lock lock(mutex_);
    entry.id = nextId_++;
    entries_.push_back(entry);
    ++revision_;
    return entry.id;
}

bool AddressList::remove(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const AddressEntry& e) { return e.id == id; }), entries_.end());
    if (entries_.size() == before) {
        return false;
    }
    ++revision_;
    return true;
}

bool AddressList::update(const AddressEntry& entry) {
    std::scoped_lock lock(mutex_);
    for (auto& existing : entries_) {
        if (existing.id == entry.id) {
            existing = entry;
            ++revision_;
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
            ++revision_;
            return true;
        }
    }
    return false;
}

std::vector<AddressEntry> AddressList::snapshot() const {
    std::scoped_lock lock(mutex_);
    return entries_;
}

std::uint64_t AddressList::revision() const {
    std::scoped_lock lock(mutex_);
    return revision_;
}

void AddressList::replace(std::vector<AddressEntry> entries) {
    std::scoped_lock lock(mutex_);
    entries_ = std::move(entries);
    nextId_ = 1;
    for (const auto& entry : entries_) {
        nextId_ = std::max(nextId_, entry.id + 1);
    }
    ++revision_;
}

} // namespace ire::domain

