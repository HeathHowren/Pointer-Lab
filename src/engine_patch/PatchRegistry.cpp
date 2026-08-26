#include "engine_patch/PatchRegistry.h"

#include "infra/Logger.h"

#include <algorithm>

namespace ire::engine_patch {

namespace {

using Id = infra::Result<std::uint64_t>;

} // namespace

PatchRegistry::PatchRegistry(domain::TargetSession& session) : session_(session) {}

bool PatchRegistry::overlapsLocked(std::uintptr_t address, std::size_t size, std::uint64_t ignoreId) const {
    const auto end = address + size;
    return std::any_of(patches_.begin(), patches_.end(), [&](const Patch& patch) {
        return patch.id != ignoreId && address < patch.end() && patch.address < end;
    });
}

Id PatchRegistry::apply(std::uintptr_t address, std::vector<std::uint8_t> bytes, std::string description,
                        std::string originalText) {
    if (bytes.empty()) {
        return Id::fail("A patch needs at least one byte.");
    }
    if (!session_.attached()) {
        return Id::fail("No target process is attached.");
    }

    {
        std::scoped_lock lock(mutex_);
        if (overlapsLocked(address, bytes.size(), 0)) {
            return Id::fail("That range overlaps a patch that is already recorded. Remove the existing patch "
                            "first -- otherwise this patch's \"original\" bytes would be the other patch's "
                            "replacement, and disabling them in the wrong order would leave code that never "
                            "existed.");
        }
    }

    // Read the originals *before* writing, and require the full length. A short
    // read here would record a truncated original, and disabling the patch
    // later would restore only part of it.
    auto original = session_.readBytes(address, bytes.size());
    if (!original) {
        return Id::fail("Could not read the original bytes at " + domain::toHex(address) + ": " + original.error(),
                        original.code());
    }
    if (original.value().size() != bytes.size()) {
        return Id::fail("Only " + std::to_string(original.value().size()) + " of " + std::to_string(bytes.size()) +
                        " bytes at " + domain::toHex(address) +
                        " could be read, so the original could not be recorded in full.");
    }

    if (auto written = session_.writeBytes(address, bytes); !written) {
        return Id::fail("Could not patch " + domain::toHex(address) + ": " + written.error(), written.code());
    }

    std::scoped_lock lock(mutex_);
    Patch patch;
    patch.id = nextId_++;
    patch.address = address;
    patch.originalBytes = std::move(original.value());
    patch.patchedBytes = std::move(bytes);
    patch.description = std::move(description);
    patch.originalText = std::move(originalText);
    patch.enabled = true;

    infra::Logger::instance().info("Patched " + std::to_string(patch.patchedBytes.size()) + " byte(s) at " +
                                   domain::toHex(address) + " (" + patch.description + "). Original: " +
                                   domain::bytesToHex(patch.originalBytes) + ".");

    const auto id = patch.id;
    patches_.push_back(std::move(patch));
    return Id::ok(id);
}

infra::Result<void> PatchRegistry::setEnabled(std::uint64_t id, bool enabled) {
    std::vector<std::uint8_t> bytes;
    std::uintptr_t address{};

    {
        std::scoped_lock lock(mutex_);
        const auto patch = std::find_if(patches_.begin(), patches_.end(),
                                        [id](const Patch& p) { return p.id == id; });
        if (patch == patches_.end()) {
            return infra::Result<void>::fail("That patch is no longer in the list.");
        }
        if (patch->enabled == enabled) {
            return infra::Result<void>::ok();
        }
        address = patch->address;
        bytes = enabled ? patch->patchedBytes : patch->originalBytes;
    }

    if (!session_.attached()) {
        return infra::Result<void>::fail("No target process is attached.");
    }
    if (auto written = session_.writeBytes(address, bytes); !written) {
        return infra::Result<void>::fail("Could not " + std::string(enabled ? "re-apply" : "restore") + " " +
                                             domain::toHex(address) + ": " + written.error(),
                                         written.code());
    }

    std::scoped_lock lock(mutex_);
    const auto patch = std::find_if(patches_.begin(), patches_.end(), [id](const Patch& p) { return p.id == id; });
    // Removed while the write was in flight. The bytes are written either way;
    // there is simply no record left to update.
    if (patch != patches_.end()) {
        patch->enabled = enabled;
    }
    infra::Logger::instance().info(std::string(enabled ? "Re-applied" : "Restored") + " the patch at " +
                                   domain::toHex(address) + ".");
    return infra::Result<void>::ok();
}

infra::Result<void> PatchRegistry::remove(std::uint64_t id) {
    if (auto restored = setEnabled(id, false); !restored) {
        // Deliberately not removed. A patch whose original bytes could not be
        // put back is exactly the one the user still needs a record of.
        return restored;
    }

    std::scoped_lock lock(mutex_);
    const auto removed = std::remove_if(patches_.begin(), patches_.end(), [id](const Patch& p) { return p.id == id; });
    patches_.erase(removed, patches_.end());
    return infra::Result<void>::ok();
}

infra::Result<void> PatchRegistry::restoreAll() {
    std::vector<std::uint64_t> enabled;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& patch : patches_) {
            if (patch.enabled) {
                enabled.push_back(patch.id);
            }
        }
    }

    std::size_t failures{};
    for (const auto id : enabled) {
        if (auto restored = setEnabled(id, false); !restored) {
            // Keep going. One unwritable page must not strand every other
            // patch in the applied state.
            infra::Logger::instance().error("Could not restore a patch: " + restored.error());
            ++failures;
        }
    }

    if (failures != 0) {
        return infra::Result<void>::fail(std::to_string(failures) + " of " + std::to_string(enabled.size()) +
                                         " patches could not be restored; see the log.");
    }
    return infra::Result<void>::ok();
}

void PatchRegistry::forgetAll() {
    std::scoped_lock lock(mutex_);
    if (!patches_.empty()) {
        const auto stillApplied =
            std::count_if(patches_.begin(), patches_.end(), [](const Patch& p) { return p.enabled; });
        infra::Logger::instance().info("Forgetting " + std::to_string(patches_.size()) + " patch record(s); " +
                                       std::to_string(stillApplied) +
                                       " were still applied and remain in effect in the target.");
    }
    patches_.clear();
}

std::vector<Patch> PatchRegistry::patches() const {
    std::scoped_lock lock(mutex_);
    return patches_;
}

std::optional<Patch> PatchRegistry::find(std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    const auto patch = std::find_if(patches_.begin(), patches_.end(), [id](const Patch& p) { return p.id == id; });
    if (patch == patches_.end()) {
        return std::nullopt;
    }
    return *patch;
}

bool PatchRegistry::covers(std::uintptr_t address) const {
    std::scoped_lock lock(mutex_);
    return std::any_of(patches_.begin(), patches_.end(), [address](const Patch& patch) {
        return patch.enabled && address >= patch.address && address < patch.end();
    });
}

bool PatchRegistry::drifted(const Patch& patch) const {
    if (!session_.attached()) {
        return false;
    }
    auto current = session_.readBytes(patch.address, patch.size());
    if (!current || current.value().size() != patch.size()) {
        return false;
    }
    const auto& expected = patch.enabled ? patch.patchedBytes : patch.originalBytes;
    return current.value() != expected;
}

} // namespace ire::engine_patch
