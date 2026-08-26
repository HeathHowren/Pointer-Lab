// PointerLabSpeed -- the speed-hack payload.
//
// Games do not measure time; they ask Windows what time it is and work out how
// much has passed since they last asked. Every animation, every physics step,
// every cooldown is that delta multiplied by something. So a "speed hack" is
// not a hack on the game at all: it is a hook on the four functions the game
// asks, returning a clock that runs at a different rate.
//
// The same source is built twice, once for each architecture, because the DLL
// has to match the process it is loaded into. There is nothing bitness-specific
// in it -- the import tables it walks have the same shape either way -- so a
// divergence between the two builds would be a compiler's doing, not ours.
//
// -------------------------------------------------------------------------
// Why the clock is rebased rather than multiplied
// -------------------------------------------------------------------------
//
// The obvious implementation, `return real * scale`, is wrong in a way that is
// obvious the moment you try it: the instant the scale changes, the clock jumps
// by hours, and when the scale is lowered again it jumps *backwards*. A game
// that sees time go backwards does not slow down; it divides by a negative
// delta and detonates.
//
// So each clock keeps a pair -- the real value when the rate last changed, and
// the fake value being reported at that moment -- and answers
//
//     fake = baseFake + (real - baseReal) * scale
//
// Changing the rate re-reads both bases first. The reported clock is therefore
// continuous and monotonic across every scale change, which is the property the
// game actually depends on.
//
// -------------------------------------------------------------------------
// Why the imports are patched rather than the functions
// -------------------------------------------------------------------------
//
// An inline hook overwrites the first instructions of the real function with a
// jump, which means decoding them to know how many to copy, allocating a
// trampoline, and getting all of it right while other threads are executing the
// bytes being rewritten.
//
// Patching the import address table does not touch a single instruction. Every
// call the game compiled as `call [__imp_QueryPerformanceCounter]` reads its
// destination from a table in the game's own image, and writing a different
// address into that table redirects every one of them atomically, with an undo
// that is just writing the old value back.
//
// What it misses is a call that does not go through the table: an address
// obtained from GetProcAddress at runtime, or a call from inside a system DLL.
// That is a real limit and it is reported rather than hidden -- pl_hook_count
// says how many entries were actually redirected, and zero means this game does
// not ask for the time the way this hook listens for.

#include <Windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// The control block. Pointer Lab finds these by name in the export directory
// and reads and writes them with ReadProcessMemory / WriteProcessMemory, which
// is why they are exported variables rather than functions: a remote thread can
// pass one pointer-sized argument, and a scale is a double.
// ---------------------------------------------------------------------------

CRITICAL_SECTION g_lock;
bool g_lockReady = false;

using RealQpc = BOOL(WINAPI*)(LARGE_INTEGER*);
using RealTick32 = DWORD(WINAPI*)();
using RealTick64 = ULONGLONG(WINAPI*)();

RealQpc g_realQpc = nullptr;
RealTick32 g_realGetTickCount = nullptr;
RealTick64 g_realGetTickCount64 = nullptr;
RealTick32 g_realTimeGetTime = nullptr;

// One clock. `read` is how the real value is obtained when the rate changes and
// the bases have to be recomputed; without it a rate change could only take
// effect at the next call, and the two clocks would have drifted apart by then.
struct Clock {
    LONG64 (*read)();
    LONG64 baseReal;
    LONG64 baseFake;
    bool started;
};

LONG64 readQpc() {
    LARGE_INTEGER value{};
    if (g_realQpc != nullptr && g_realQpc(&value)) {
        return value.QuadPart;
    }
    return 0;
}
LONG64 readTick32() {
    return g_realGetTickCount != nullptr ? static_cast<LONG64>(g_realGetTickCount()) : 0;
}
LONG64 readTick64() {
    return g_realGetTickCount64 != nullptr ? static_cast<LONG64>(g_realGetTickCount64()) : 0;
}
LONG64 readTimeGetTime() {
    return g_realTimeGetTime != nullptr ? static_cast<LONG64>(g_realTimeGetTime()) : 0;
}

Clock g_qpc{&readQpc, 0, 0, false};
Clock g_tick32{&readTick32, 0, 0, false};
Clock g_tick64{&readTick64, 0, 0, false};
Clock g_mmTime{&readTimeGetTime, 0, 0, false};

Clock* const g_clocks[]{&g_qpc, &g_tick32, &g_tick64, &g_mmTime};

// Must be called with the lock held.
LONG64 fakeOf(Clock& clock, LONG64 real, double scale) {
    if (!clock.started) {
        clock.baseReal = real;
        clock.baseFake = real;
        clock.started = true;
        return real;
    }
    // GetTickCount and timeGetTime are 32-bit and wrap after 49 days, and a
    // machine that has been up that long is not a reason to report a negative
    // delta. Rebasing on a backwards step costs one wrong frame instead.
    if (real < clock.baseReal) {
        clock.baseReal = real;
    }
    const double elapsed = static_cast<double>(real - clock.baseReal) * scale;
    return clock.baseFake + static_cast<LONG64>(elapsed);
}

} // namespace

extern "C" {

// Written by Pointer Lab. The worker thread notices the change and rebases.
__declspec(dllexport) volatile double pl_requested_scale = 1.0;
// Read by Pointer Lab: what is actually in force, which is not the same thing
// until the worker has run.
__declspec(dllexport) volatile double pl_applied_scale = 1.0;
// How many import entries were redirected. Zero means the hook found nothing to
// hook, which is worth knowing and would otherwise look like a hook that works
// and does nothing.
__declspec(dllexport) volatile LONG pl_hook_count = 0;
// Set to 1 by Pointer Lab to put every patched entry back. The DLL stays
// loaded: unloading a module while another thread might be executing inside it
// is a crash, and there is no way to prove none is.
__declspec(dllexport) volatile LONG pl_unhook = 0;
// So Pointer Lab can tell "the payload is loaded and running" from "the export
// resolved but the worker never started".
__declspec(dllexport) volatile LONG pl_alive = 0;

} // extern "C"

namespace {

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

BOOL WINAPI hookedQueryPerformanceCounter(LARGE_INTEGER* out) {
    LARGE_INTEGER real{};
    if (g_realQpc == nullptr) {
        return FALSE;
    }
    const BOOL ok = g_realQpc(&real);
    if (!ok || out == nullptr) {
        return ok;
    }
    EnterCriticalSection(&g_lock);
    out->QuadPart = fakeOf(g_qpc, real.QuadPart, pl_applied_scale);
    LeaveCriticalSection(&g_lock);
    return TRUE;
}

DWORD WINAPI hookedGetTickCount() {
    if (g_realGetTickCount == nullptr) {
        return 0;
    }
    const LONG64 real = static_cast<LONG64>(g_realGetTickCount());
    EnterCriticalSection(&g_lock);
    const LONG64 fake = fakeOf(g_tick32, real, pl_applied_scale);
    LeaveCriticalSection(&g_lock);
    // Truncated to 32 bits exactly as the real one is, wrap included.
    return static_cast<DWORD>(static_cast<ULONG64>(fake) & 0xFFFFFFFFull);
}

ULONGLONG WINAPI hookedGetTickCount64() {
    if (g_realGetTickCount64 == nullptr) {
        return 0;
    }
    const LONG64 real = static_cast<LONG64>(g_realGetTickCount64());
    EnterCriticalSection(&g_lock);
    const LONG64 fake = fakeOf(g_tick64, real, pl_applied_scale);
    LeaveCriticalSection(&g_lock);
    return static_cast<ULONGLONG>(fake);
}

DWORD WINAPI hookedTimeGetTime() {
    if (g_realTimeGetTime == nullptr) {
        return 0;
    }
    const LONG64 real = static_cast<LONG64>(g_realTimeGetTime());
    EnterCriticalSection(&g_lock);
    const LONG64 fake = fakeOf(g_mmTime, real, pl_applied_scale);
    LeaveCriticalSection(&g_lock);
    return static_cast<DWORD>(static_cast<ULONG64>(fake) & 0xFFFFFFFFull);
}

struct HookTarget {
    const char* name;
    void* replacement;
    void** real;
};

const HookTarget g_targets[]{
    {"QueryPerformanceCounter", reinterpret_cast<void*>(&hookedQueryPerformanceCounter),
     reinterpret_cast<void**>(&g_realQpc)},
    {"GetTickCount", reinterpret_cast<void*>(&hookedGetTickCount),
     reinterpret_cast<void**>(&g_realGetTickCount)},
    {"GetTickCount64", reinterpret_cast<void*>(&hookedGetTickCount64),
     reinterpret_cast<void**>(&g_realGetTickCount64)},
    {"timeGetTime", reinterpret_cast<void*>(&hookedTimeGetTime),
     reinterpret_cast<void**>(&g_realTimeGetTime)},
};

// ---------------------------------------------------------------------------
// Import table patching
// ---------------------------------------------------------------------------

// Every entry redirected, so it can be put back. Bounded rather than growable:
// this runs inside somebody else's process and a heap allocation from a hook
// path is a risk with no upside. A game with more than this many redirected
// timing imports does not exist.
constexpr int maxPatches = 2048;

struct Patch {
    void** slot;
    void* original;
};

Patch g_patches[maxPatches];
int g_patchCount = 0;

HMODULE g_self = nullptr;

// The modules that implement the clocks, which must never be patched.
//
// This is not caution, it is a correctness requirement, and getting it wrong
// looks like nothing until the first call. kernel32 is mostly a set of stubs
// that reach the real implementation in kernelbase *through kernel32's own
// import table* -- kernel32!GetTickCount64 is little more than
// `jmp [__imp_GetTickCount64]`. Redirect that entry and the hook's call to the
// "real" function lands straight back in the hook, and the target dies of a
// stack overflow on the first frame that asks the time.
//
// winmm is the same shape over winmmbase, and ntdll is where all of it
// bottoms out.
bool isClockModule(const wchar_t* name) {
    if (name == nullptr) {
        return false;
    }
    static const wchar_t* const names[]{
        L"ntdll.dll", L"kernel32.dll", L"kernelbase.dll", L"winmm.dll", L"winmmbase.dll",
    };
    for (const wchar_t* candidate : names) {
        if (_wcsicmp(name, candidate) == 0) {
            return true;
        }
    }
    // The API-set shims forward into the same place, under a hundred different
    // names that are not worth enumerating.
    return _wcsnicmp(name, L"api-ms-", 7) == 0 || _wcsnicmp(name, L"ext-ms-", 7) == 0;
}

bool alreadyPatched(void** slot) {
    for (int i = 0; i < g_patchCount; ++i) {
        if (g_patches[i].slot == slot) {
            return true;
        }
    }
    return false;
}

void writeSlot(void** slot, void* value) {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
        return;
    }
    *slot = value;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
}

void patchModule(HMODULE module) {
    // Our own imports are left alone. Hooking them would send our hook's call
    // to the real function straight back into the hook.
    if (module == g_self || module == nullptr) {
        return;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return;
    }

    const auto* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        // OriginalFirstThunk still holds the names after the loader has
        // overwritten FirstThunk with addresses. A bound import has no original
        // thunk, in which case the names are gone and there is nothing to match
        // on -- that module is simply skipped.
        if (descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        const auto* original =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);

        for (; original->u1.AddressOfData != 0; ++thunk, ++original) {
            if (IMAGE_SNAP_BY_ORDINAL(original->u1.Ordinal)) {
                continue;
            }
            const auto* byName =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
            for (const auto& target : g_targets) {
                if (_stricmp(byName->Name, target.name) != 0) {
                    continue;
                }
                auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot == target.replacement || alreadyPatched(slot)) {
                    break;
                }
                if (g_patchCount >= maxPatches) {
                    break;
                }
                g_patches[g_patchCount].slot = slot;
                g_patches[g_patchCount].original = *slot;
                ++g_patchCount;
                writeSlot(slot, target.replacement);
                InterlockedIncrement(&pl_hook_count);
                break;
            }
        }
    }
}

void patchEverything() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!isClockModule(entry.szModule)) {
                patchModule(entry.hModule);
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void unpatchEverything() {
    // In reverse, so a slot patched twice (which alreadyPatched prevents, but
    // belt and braces) ends up holding the value it started with.
    for (int i = g_patchCount - 1; i >= 0; --i) {
        writeSlot(g_patches[i].slot, g_patches[i].original);
    }
    g_patchCount = 0;
    InterlockedExchange(&pl_hook_count, 0);
}

void applyScale(double next) {
    EnterCriticalSection(&g_lock);
    const double current = pl_applied_scale;
    for (Clock* clock : g_clocks) {
        if (!clock->started) {
            continue;
        }
        // Freeze this clock at the value it is reporting *now*, at the old
        // rate, before the new rate applies. This is the whole reason the
        // change is a rebase and not an assignment.
        const LONG64 real = clock->read();
        clock->baseFake = fakeOf(*clock, real, current);
        clock->baseReal = real;
    }
    pl_applied_scale = next;
    LeaveCriticalSection(&g_lock);
}

// The implementation, not the stub that reaches it.
FARPROC realFunction(const char* name) {
    if (HMODULE base = GetModuleHandleW(L"kernelbase.dll"); base != nullptr) {
        if (FARPROC found = GetProcAddress(base, name); found != nullptr) {
            return found;
        }
    }
    return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
}

DWORD WINAPI worker(LPVOID) {
    // Everything the DLL does beyond storing a pointer happens here rather than
    // in DllMain. Loading a snapshot, calling VirtualProtect and creating a
    // thread are all forbidden under the loader lock, and DllMain holds it.
    // kernelbase before kernel32: kernel32's entry is a stub that reaches the
    // implementation through its own import table, and calling the
    // implementation directly means the hook's downstream call cannot be
    // redirected by anything, whether or not isClockModule() names every module
    // that matters.
    g_realQpc = reinterpret_cast<RealQpc>(reinterpret_cast<void*>(realFunction("QueryPerformanceCounter")));
    g_realGetTickCount = reinterpret_cast<RealTick32>(reinterpret_cast<void*>(realFunction("GetTickCount")));
    g_realGetTickCount64 =
        reinterpret_cast<RealTick64>(reinterpret_cast<void*>(realFunction("GetTickCount64")));
    // winmm may not be loaded; a game that never calls timeGetTime has no
    // reason to have it, and loading it ourselves would be changing the target
    // more than the hook does.
    if (HMODULE winmm = GetModuleHandleW(L"winmmbase.dll"); winmm != nullptr) {
        g_realTimeGetTime = reinterpret_cast<RealTick32>(
            reinterpret_cast<void*>(GetProcAddress(winmm, "timeGetTime")));
    }
    if (g_realTimeGetTime == nullptr) {
        if (HMODULE winmm = GetModuleHandleW(L"winmm.dll"); winmm != nullptr) {
            g_realTimeGetTime = reinterpret_cast<RealTick32>(
                reinterpret_cast<void*>(GetProcAddress(winmm, "timeGetTime")));
        }
    }

    patchEverything();
    InterlockedExchange(&pl_alive, 1);

    double applied = pl_applied_scale;
    int sinceRescan = 0;
    for (;;) {
        if (InterlockedCompareExchange(&pl_unhook, 0, 0) != 0) {
            applyScale(1.0);
            unpatchEverything();
            InterlockedExchange(&pl_unhook, 0);
            InterlockedExchange(&pl_alive, 0);
            return 0;
        }

        const double requested = pl_requested_scale;
        if (requested != applied && requested > 0.0) {
            applyScale(requested);
            applied = requested;
        }

        // Modules loaded after the injection -- a renderer, an anti-cheat, a
        // level's own DLL -- have import tables of their own, and a hook that
        // only ever ran once would miss them.
        if (++sinceRescan >= 60) {
            sinceRescan = 0;
            if (g_realTimeGetTime == nullptr) {
                HMODULE winmm = GetModuleHandleW(L"winmmbase.dll");
                if (winmm == nullptr) {
                    winmm = GetModuleHandleW(L"winmm.dll");
                }
                if (winmm != nullptr) {
                    g_realTimeGetTime = reinterpret_cast<RealTick32>(
                        reinterpret_cast<void*>(GetProcAddress(winmm, "timeGetTime")));
                }
            }
            patchEverything();
        }
        Sleep(8);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        const HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // The process is going away; putting the imports back would only race
        // with threads that are already being torn down.
        if (g_lockReady) {
            DeleteCriticalSection(&g_lock);
            g_lockReady = false;
        }
    }
    return TRUE;
}
