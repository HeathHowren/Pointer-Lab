#include "services/UiCommands.h"

#include <atomic>

namespace ire::services {

namespace {

// Written once while the frontend is being built and read from the script
// worker thread. Atomic rather than a plain pointer because those are two
// different threads, however unlikely the race is in practice.
std::atomic<UiCommands*> g_commands{nullptr};

} // namespace

UiCommands* uiCommands() {
    return g_commands.load(std::memory_order_acquire);
}

void setUiCommands(UiCommands* commands) {
    g_commands.store(commands, std::memory_order_release);
}

} // namespace ire::services
