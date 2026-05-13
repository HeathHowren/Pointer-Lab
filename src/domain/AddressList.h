#pragma once

#include "domain/Domain.h"

#include <mutex>
#include <vector>

namespace ire::domain {

class AddressList {
public:
    std::uint64_t add(AddressEntry entry);
    bool remove(std::uint64_t id);
    bool update(const AddressEntry& entry);
    bool setFrozen(std::uint64_t id, bool frozen);
    std::vector<AddressEntry> snapshot() const;
    void replace(std::vector<AddressEntry> entries);

private:
    mutable std::mutex mutex_;
    std::uint64_t nextId_{1};
    std::vector<AddressEntry> entries_;
};

} // namespace ire::domain

