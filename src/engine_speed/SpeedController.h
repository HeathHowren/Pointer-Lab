#pragma once

#include "domain/TargetSession.h"
#include "engine_inject/Injector.h"
#include "infra/Result.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace ire::engine_speed {

struct SpeedStatus {
    // True once the payload is loaded and its control block has been located.
    bool loaded{};
    // True once the payload's worker thread has started and patched what it
    // could find. Distinct from `loaded`, because a payload whose thread never
    // ran resolves its exports perfectly well and does nothing.
    bool running{};
    double requested{1.0};
    double applied{1.0};
    // How many import entries the payload actually redirected. Zero after a
    // successful injection is the honest and important case: this game does not
    // ask for the time through its import table.
    std::uint32_t hookedImports{};
};

// The speed hack: not a hack on the game, but on the clock it reads.
//
// Everything a game does per frame is a delta multiplied by something -- how far
// to move, how much of a cooldown has elapsed, how far through an animation to
// be. The game gets that delta by asking Windows the time twice. So changing
// the *rate* of the four clocks it can ask changes the rate of the game, with
// no knowledge of the game required at all.
//
// This class is the near side of that: it injects the payload, finds its
// exported control block by parsing the payload's own export directory out of
// the target, and reads and writes it. The far side is src/payload/SpeedHook.cpp.
class SpeedController {
public:
    SpeedController(domain::TargetSession& session, engine_inject::Injector& injector);

    // Loads the payload that matches the target's architecture and locates its
    // control block. Safe to call again: an already-loaded payload is found
    // rather than loaded twice.
    infra::Result<void> load();
    // Refuses a scale outside the supported range rather than clamping it: a
    // silently clamped 1000x looks exactly like a hook that is not working.
    infra::Result<void> setScale(double scale);
    // Back to normal speed and every patched import restored. The payload stays
    // loaded -- unloading a module while a thread may be executing inside it is
    // a crash with no way to prove it will not happen.
    infra::Result<void> reset();
    [[nodiscard]] SpeedStatus status() const;
    // Forgets the addresses of the control block. Called on detach: they name
    // nothing in the next process.
    void forget();

    // Below this the game is stopped rather than slow, and above it a single
    // frame advances the world far enough that collision detection steps
    // straight through walls. Both ends are the point at which "slower" and
    // "faster" stop being what happens.
    static constexpr double minScale = 0.05;
    static constexpr double maxScale = 20.0;

    // Where the payload for a target of that width is expected. Exposed so the
    // UI can name the missing file rather than reporting that injection failed.
    [[nodiscard]] static std::filesystem::path payloadPath(domain::Bitness bitness);
    [[nodiscard]] static std::wstring payloadModuleName(domain::Bitness bitness);

private:
    // Reads the payload's export directory out of the target and records where
    // each control variable lives.
    infra::Result<void> locateControlBlock(std::uintptr_t moduleBase);

    domain::TargetSession& session_;
    engine_inject::Injector& injector_;

    mutable std::mutex mutex_;
    std::uintptr_t requestedScale_{};
    std::uintptr_t appliedScale_{};
    std::uintptr_t hookCount_{};
    std::uintptr_t unhook_{};
    std::uintptr_t alive_{};
};

} // namespace ire::engine_speed
