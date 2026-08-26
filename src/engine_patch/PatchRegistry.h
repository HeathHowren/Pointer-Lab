#pragma once

#include "domain/TargetSession.h"
#include "infra/Result.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ire::engine_patch {

struct Patch {
    std::uint64_t id{};
    std::uintptr_t address{};
    // What was there before, captured at the moment the patch was first
    // applied. This is the whole reason the registry exists.
    std::vector<std::uint8_t> originalBytes;
    std::vector<std::uint8_t> patchedBytes;
    std::string description;
    // Disassembly of the original bytes, so the list can say what was replaced
    // rather than only showing hex.
    std::string originalText;
    bool enabled{};

    [[nodiscard]] std::size_t size() const { return originalBytes.size(); }
    [[nodiscard]] std::uintptr_t end() const { return address + originalBytes.size(); }
};

// Every byte Pointer Lab writes into a target's code, and what was there
// before.
//
// Without this, patching is one-way: "replace with code that does nothing" is
// easy to do and impossible to take back short of restarting the target. That
// is a bad property for a tool people are learning with, where the whole point
// of a patch is often to see the difference it makes and then put it back.
//
// It is also the substrate an [ENABLE]/[DISABLE] script model needs: a script
// that enables a hook is a set of patches, and disabling it is restoring them
// in reverse.
class PatchRegistry {
public:
    explicit PatchRegistry(domain::TargetSession& session);

    // Captures the bytes currently at `address`, writes `bytes` over them, and
    // records the pair. `originalText` is display only.
    //
    // Refuses when the range overlaps an existing patch. That is not
    // fussiness: the second patch's "original" bytes would be the first
    // patch's *patched* bytes, so disabling them in the wrong order would leave
    // a mixture of the two that was never real code. Refusing is the only
    // answer that keeps every recorded original genuinely original.
    infra::Result<std::uint64_t> apply(std::uintptr_t address, std::vector<std::uint8_t> bytes,
                                       std::string description, std::string originalText = {});

    // Writes the patched bytes (enable) or the originals (disable). A no-op
    // when the patch is already in that state.
    infra::Result<void> setEnabled(std::uint64_t id, bool enabled);

    // Restores the original bytes and forgets the patch.
    infra::Result<void> remove(std::uint64_t id);

    // Disables every enabled patch, continuing past failures so one unwritable
    // page cannot strand the rest. The error names how many could not be
    // restored.
    infra::Result<void> restoreAll();

    // Drops every record *without* touching the target. For detaching: the
    // process keeps running with whatever is currently applied, and we no
    // longer have a handle to change it. The caller is expected to tell the
    // user that.
    void forgetAll();

    [[nodiscard]] std::vector<Patch> patches() const;
    [[nodiscard]] std::optional<Patch> find(std::uint64_t id) const;

    // True when an enabled patch covers this address, so a listing can mark it
    // as modified rather than presenting patched bytes as the program's own.
    [[nodiscard]] bool covers(std::uintptr_t address) const;

    // Bytes at `address` that differ from what this registry last wrote there.
    // Something else changed the code -- the target rewriting itself, an
    // anti-tamper check, or a write that bypassed the registry.
    [[nodiscard]] bool drifted(const Patch& patch) const;

private:
    // Expects mutex_ to be held.
    [[nodiscard]] bool overlapsLocked(std::uintptr_t address, std::size_t size, std::uint64_t ignoreId) const;

    domain::TargetSession& session_;
    mutable std::mutex mutex_;
    std::vector<Patch> patches_;
    std::uint64_t nextId_{1};
};

} // namespace ire::engine_patch
