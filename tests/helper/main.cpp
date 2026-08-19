// A minimal, scriptable target process for the Win32 integration tests.
//
// It owns one 32-bit value on a dedicated page, reachable through a two-level
// pointer chain rooted in this module's own data section, and runs a background
// loop calling tick() so the debugger tests have a hot function to break on.
// Commands arrive on stdin so a test can change state at an exact point.
//
// Protocol (one line each, always flushed):
//   out  ADDR <hex>   address of the value
//   out  ROOT <hex>   address of the module-global chain root
//   out  TICK <hex>   address of tick(), called continuously by a worker thread
//   in   SET <int>    store a new value        -> out  OK <int>
//   in   GET          read the current value   -> out  VAL <int>
//   in   TICKS        read the tick counter    -> out  TICKCOUNT <n>
//   in   QUIT         exit cleanly

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Leaf {
    volatile std::int32_t* value;
};

struct Node {
    Leaf* next;
};

// Deliberately a module global: a pointer scan should find this as
// "pointerlab_test_helper.exe+<offset>" and re-resolve it after a restart.
Node* g_root = nullptr;

volatile long long g_ticks = 0;
volatile LONG g_running = 1;

} // namespace

// noinline so the address printed below is a real function with a real first
// instruction for a breakpoint to replace.
__declspec(noinline) void tick() {
    g_ticks += 1;
}

static DWORD WINAPI worker(LPVOID) {
    while (InterlockedCompareExchange(&g_running, 1, 1) != 0) {
        // A burst then a yield: fast enough to prove a breakpoint re-arms and
        // keeps firing, without pinning a core for the whole test run.
        for (int i = 0; i < 100; ++i) {
            tick();
        }
        Sleep(1);
    }
    return 0;
}

int main() {
    auto* slot = static_cast<volatile std::int32_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (slot == nullptr) {
        std::fprintf(stderr, "VirtualAlloc failed\n");
        return 1;
    }
    *slot = 0x5AFE1234;

    static Leaf leaf{};
    static Node node{};
    leaf.value = slot;
    node.next = &leaf;
    g_root = &node;

    std::printf("ADDR %llX\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slot)));
    std::printf("ROOT %llX\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&g_root)));
    std::printf("TICK %llX\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&tick)));
    std::fflush(stdout);

    HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);

    char line[256];
    while (std::fgets(line, static_cast<int>(sizeof(line)), stdin) != nullptr) {
        if (std::strncmp(line, "SET ", 4) == 0) {
            *slot = static_cast<std::int32_t>(std::strtol(line + 4, nullptr, 10));
            std::printf("OK %ld\n", static_cast<long>(*slot));
        } else if (std::strncmp(line, "GET", 3) == 0) {
            std::printf("VAL %ld\n", static_cast<long>(*slot));
        } else if (std::strncmp(line, "TICKS", 5) == 0) {
            std::printf("TICKCOUNT %lld\n", g_ticks);
        } else if (std::strncmp(line, "QUIT", 4) == 0) {
            break;
        } else {
            std::printf("ERR\n");
        }
        std::fflush(stdout);
    }

    InterlockedExchange(&g_running, 0);
    if (thread != nullptr) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    return 0;
}
