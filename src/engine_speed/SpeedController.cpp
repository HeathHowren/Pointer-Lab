#include "engine_speed/SpeedController.h"

#include "engine_symbols/ExportResolver.h"
#include "infra/Logger.h"
#include "infra/Paths.h"

#include <cstring>
#include <optional>
#include <vector>

namespace ire::engine_speed {

namespace {

template <typename T>
using Result = infra::Result<T>;

template <typename T>
std::optional<T> readAs(const domain::TargetSession& session, std::uintptr_t address) {
    if (address == 0) {
        return std::nullopt;
    }
    auto bytes = session.readBytes(address, sizeof(T));
    if (!bytes || bytes.value().size() != sizeof(T)) {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, bytes.value().data(), sizeof(T));
    return value;
}

template <typename T>
infra::Result<void> writeAs(const domain::TargetSession& session, std::uintptr_t address, T value) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return session.writeBytes(address, bytes);
}

} // namespace

SpeedController::SpeedController(domain::TargetSession& session, engine_inject::Injector& injector)
    : session_(session), injector_(injector) {}

std::filesystem::path SpeedController::payloadPath(domain::Bitness bitness) {
    return infra::Paths::speedPayload(bitness == domain::Bitness::X64);
}

std::wstring SpeedController::payloadModuleName(domain::Bitness bitness) {
    return bitness == domain::Bitness::X64 ? L"PointerLabSpeed64.dll" : L"PointerLabSpeed32.dll";
}

infra::Result<void> SpeedController::load() {
    if (!session_.attached()) {
        return Result<void>::fail("Attach to a process before changing its speed.");
    }
    if (session_.readOnly()) {
        return Result<void>::fail("Only a read-only handle to this process could be obtained, so nothing "
                                  "can be injected into it. Run Pointer Lab as administrator.");
    }

    const auto bitness = session_.bitness();
    const auto moduleName = payloadModuleName(bitness);

    // Already there from an earlier attach in this run of the target.
    session_.refresh();
    auto base = engine_symbols::ExportResolver::moduleBase(session_, moduleName);
    if (base == 0) {
        const auto path = payloadPath(bitness);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            // Named rather than reported as an injection failure. A missing
            // payload is a broken install, and "LoadLibrary returned 0" sends
            // people looking at the game.
            return Result<void>::fail("The " + std::string(domain::bitnessName(bitness)) +
                                      " speed payload is missing: " + path.string() +
                                      ". It ships beside PointerLab.exe.");
        }
        if (auto loaded = injector_.loadLibrary(path.wstring()); !loaded) {
            return Result<void>::fail("Could not load the speed payload into the target: " + loaded.error());
        }
        session_.refresh();
        base = engine_symbols::ExportResolver::moduleBase(session_, moduleName);
        if (base == 0) {
            // LoadLibraryW returned without the module appearing, which on a
            // 32-bit target almost always means the wrong payload was built.
            return Result<void>::fail("The payload was loaded but is not in the target's module list. If the "
                                      "target is " +
                                      std::string(domain::bitnessName(bitness)) +
                                      ", check that the matching payload was built.");
        }
    }

    return locateControlBlock(base);
}

infra::Result<void> SpeedController::locateControlBlock(std::uintptr_t moduleBase) {
    const engine_symbols::ExportResolver resolver;
    auto exports = resolver.exports(session_, moduleBase);
    if (!exports) {
        return Result<void>::fail("Could not read the payload's exports: " + exports.error());
    }

    std::uintptr_t requested = 0;
    std::uintptr_t applied = 0;
    std::uintptr_t hooks = 0;
    std::uintptr_t unhook = 0;
    std::uintptr_t alive = 0;
    for (const auto& entry : exports.value()) {
        if (entry.name == "pl_requested_scale") requested = entry.address;
        else if (entry.name == "pl_applied_scale") applied = entry.address;
        else if (entry.name == "pl_hook_count") hooks = entry.address;
        else if (entry.name == "pl_unhook") unhook = entry.address;
        else if (entry.name == "pl_alive") alive = entry.address;
    }
    if (requested == 0 || applied == 0 || hooks == 0 || unhook == 0 || alive == 0) {
        return Result<void>::fail("A module with the payload's name is loaded, but it does not export the "
                                  "control block. It is not this payload.");
    }

    std::scoped_lock lock(mutex_);
    requestedScale_ = requested;
    appliedScale_ = applied;
    hookCount_ = hooks;
    unhook_ = unhook;
    alive_ = alive;
    infra::Logger::instance().info("Speed payload control block at " + domain::toHex(requested) + ".");
    return Result<void>::ok();
}

infra::Result<void> SpeedController::setScale(double scale) {
    if (!(scale >= minScale && scale <= maxScale)) {
        // Not clamped. A silently clamped request looks exactly like a hook
        // that is installed and doing nothing.
        return Result<void>::fail("Speed must be between " + std::to_string(minScale) + "x and " +
                                  std::to_string(maxScale) +
                                  "x. Below that the game is stopped rather than slow; above it a single "
                                  "frame moves things far enough to pass through walls.");
    }

    std::uintptr_t address = 0;
    {
        std::scoped_lock lock(mutex_);
        address = requestedScale_;
    }
    if (address == 0) {
        if (auto loaded = load(); !loaded) {
            return loaded;
        }
        std::scoped_lock lock(mutex_);
        address = requestedScale_;
    }
    return writeAs<double>(session_, address, scale);
}

infra::Result<void> SpeedController::reset() {
    std::uintptr_t requested = 0;
    std::uintptr_t unhook = 0;
    {
        std::scoped_lock lock(mutex_);
        requested = requestedScale_;
        unhook = unhook_;
    }
    if (requested == 0 || unhook == 0) {
        // Nothing was ever injected, so there is nothing to put back. Not an
        // error: this runs on detach whether or not the feature was used.
        return Result<void>::ok();
    }
    if (auto written = writeAs<double>(session_, requested, 1.0); !written) {
        return written;
    }
    // The payload's worker restores every import it patched and then stops.
    return writeAs<std::int32_t>(session_, unhook, 1);
}

SpeedStatus SpeedController::status() const {
    SpeedStatus status;
    std::uintptr_t requested = 0;
    std::uintptr_t applied = 0;
    std::uintptr_t hooks = 0;
    std::uintptr_t alive = 0;
    {
        std::scoped_lock lock(mutex_);
        requested = requestedScale_;
        applied = appliedScale_;
        hooks = hookCount_;
        alive = alive_;
    }
    if (requested == 0) {
        return status;
    }
    status.loaded = true;
    status.requested = readAs<double>(session_, requested).value_or(1.0);
    status.applied = readAs<double>(session_, applied).value_or(1.0);
    status.hookedImports = static_cast<std::uint32_t>(readAs<std::int32_t>(session_, hooks).value_or(0));
    status.running = readAs<std::int32_t>(session_, alive).value_or(0) != 0;
    return status;
}

void SpeedController::forget() {
    std::scoped_lock lock(mutex_);
    requestedScale_ = 0;
    appliedScale_ = 0;
    hookCount_ = 0;
    unhook_ = 0;
    alive_ = 0;
}

} // namespace ire::engine_speed
