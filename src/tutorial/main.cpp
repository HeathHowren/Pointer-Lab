// PointerLabTutorial -- nine gated lessons against a program that is honest
// about being a target.
//
// The point of a tutorial target is that the answer is checkable. Reading about
// a pointer scan teaches the words; being told "no, that address died when the
// object moved" teaches the thing. So every step here ends in a check that the
// program performs on itself, and most of those checks are deliberately
// arranged so that the plausible-but-wrong technique fails:
//
//   - Step 5 calls the writing code and requires the value not to change, so
//     freezing the value passes nothing and only removing the instruction does.
//   - Steps 6 and 7 move the object before checking, so an address found by
//     scanning is dead by the time the check reads it, and only an entry that
//     re-resolves its chain survives.
//   - Step 9 damages both objects directly and reads back immediately, so a
//     frozen value has no time to be restored and only code that distinguishes
//     the two objects can pass.
//
// Built for x86 and x64 from this one source, because pointer width is one of
// the things being taught and a reader should be able to see the same lesson
// come out four bytes wide and eight bytes wide.
//
// The values live in one 64 KB allocation with the individual values spread far
// apart inside it, rather than in adjacent globals. Adjacent globals would let
// someone who found Step 2's value read Step 3's from the next scan result,
// which is not the lesson.

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// The values being hunted
// ---------------------------------------------------------------------------

std::uint8_t* g_pool = nullptr;

volatile long* g_step1 = nullptr;
volatile int* g_step2 = nullptr;
volatile int* g_step3 = nullptr;
volatile float* g_step4Health = nullptr;
volatile double* g_step4Ammo = nullptr;
volatile int* g_step5 = nullptr;
volatile int* g_step8 = nullptr;

// Steps 6 and 7 own memory that is deliberately thrown away and allocated again,
// because "the address you wrote down is now somebody else's" is the entire
// lesson and it cannot be demonstrated in a fixed allocation.
struct Step6Object {
    volatile int health;
    int filler[15];
};

// Four hops, each in its own allocation, so that the chain the pointer scanner
// finds has to be walked rather than guessed. The filler is there so the
// offsets are not all zero -- a chain of four zero offsets teaches nothing about
// reading an offset.
struct Level4 {
    int filler[6];
    volatile int health;
};
struct Level3 {
    int filler[2];
    Level4* next;
};
struct Level2 {
    int filler[4];
    Level3* next;
};
struct Level1 {
    int filler[8];
    Level2* next;
};

Step6Object* g_step6 = nullptr;
Level1* g_step7 = nullptr;

// Step 9: two objects of the same kind, damaged by one function from one call
// site, which is what makes removing the instruction useless.
struct Entity {
    volatile int health;
    int id;
    int filler[6];
};

Entity* g_player = nullptr;
Entity* g_enemy = nullptr;

volatile LONG g_running = 1;

// ---------------------------------------------------------------------------
// The code that writes -- kept out of line and out of the folder's reach
// ---------------------------------------------------------------------------
//
// noinline because a step that says "find the instruction that writes to this"
// needs there to be one instruction rather than a copy of it at every call
// site. The link also runs with /OPT:NOICF, so two functions that happen to
// compile to the same bytes are not folded into one and every step's answer
// stays its own.

__declspec(noinline) void step2Hit(int amount) {
    *g_step2 = *g_step2 - amount;
}

__declspec(noinline) void step3Change(int amount) {
    *g_step3 = *g_step3 + amount;
}

__declspec(noinline) void step4Hit(float amount) {
    *g_step4Health = *g_step4Health - amount;
}

__declspec(noinline) void step4Shoot(double amount) {
    *g_step4Ammo = *g_step4Ammo - amount;
}

__declspec(noinline) void step5Write(int amount) {
    *g_step5 = *g_step5 - amount;
}

__declspec(noinline) void step6Write(int amount) {
    g_step6->health = g_step6->health - amount;
}

__declspec(noinline) void step7Write(int amount) {
    g_step7->next->next->next->health = g_step7->next->next->next->health - amount;
}

__declspec(noinline) void step8Hit() {
    *g_step8 = *g_step8 - 1;
}

// One function, one write instruction, two objects. This is the whole of Step 9.
__declspec(noinline) void damage(Entity* entity, int amount) {
    entity->health = entity->health - amount;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

int randomBetween(int low, int high) {
    return low + std::rand() % (high - low + 1);
}

template <typename T>
T* poolAt(std::size_t offset) {
    return reinterpret_cast<T*>(g_pool + offset);
}

void allocateStep6() {
    if (g_step6 != nullptr) {
        VirtualFree(g_step6, 0, MEM_RELEASE);
    }
    g_step6 = static_cast<Step6Object*>(
        VirtualAlloc(nullptr, sizeof(Step6Object), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_step6 != nullptr) {
        g_step6->health = randomBetween(100, 900);
    }
}

void freeStep7() {
    if (g_step7 == nullptr) {
        return;
    }
    Level2* level2 = g_step7->next;
    Level3* level3 = level2 != nullptr ? level2->next : nullptr;
    Level4* level4 = level3 != nullptr ? level3->next : nullptr;
    if (level4 != nullptr) {
        VirtualFree(level4, 0, MEM_RELEASE);
    }
    if (level3 != nullptr) {
        VirtualFree(level3, 0, MEM_RELEASE);
    }
    if (level2 != nullptr) {
        VirtualFree(level2, 0, MEM_RELEASE);
    }
    VirtualFree(g_step7, 0, MEM_RELEASE);
    g_step7 = nullptr;
}

template <typename T>
T* allocateOne() {
    return static_cast<T*>(VirtualAlloc(nullptr, sizeof(T), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

void allocateStep7() {
    freeStep7();
    auto* level4 = allocateOne<Level4>();
    auto* level3 = allocateOne<Level3>();
    auto* level2 = allocateOne<Level2>();
    auto* level1 = allocateOne<Level1>();
    if (level4 == nullptr || level3 == nullptr || level2 == nullptr || level1 == nullptr) {
        return;
    }
    level4->health = randomBetween(100, 900);
    level3->next = level4;
    level2->next = level3;
    level1->next = level2;
    g_step7 = level1;
}

bool allocateEverything() {
    g_pool = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_pool == nullptr) {
        return false;
    }
    // A page apart, so a scan that found one value does not hand over the next
    // one for free in the same result window.
    g_step1 = poolAt<volatile long>(0x0000);
    g_step2 = poolAt<volatile int>(0x1000);
    g_step3 = poolAt<volatile int>(0x2000);
    g_step4Health = poolAt<volatile float>(0x3000);
    g_step4Ammo = poolAt<volatile double>(0x4000);
    g_step5 = poolAt<volatile int>(0x5000);
    g_step8 = poolAt<volatile int>(0x6000);
    g_player = poolAt<Entity>(0x7000);
    g_enemy = poolAt<Entity>(0x8000);

    *g_step1 = 0;
    *g_step2 = randomBetween(100, 999);
    *g_step3 = randomBetween(300, 700);
    *g_step4Health = static_cast<float>(randomBetween(500, 900)) / 10.0f;
    *g_step4Ammo = static_cast<double>(randomBetween(500, 900)) / 10.0;
    *g_step5 = randomBetween(100, 999);
    *g_step8 = 100;

    g_player->health = 1000;
    g_player->id = 1;
    g_enemy->health = 1000;
    g_enemy->id = 2;

    allocateStep6();
    allocateStep7();
    return g_step6 != nullptr && g_step7 != nullptr;
}

// The background thread of Step 9. It exists so the write is happening
// constantly and from a thread other than the one handling the buttons, which
// is how it happens in a game and is the reason a hit count climbs on its own.
DWORD WINAPI fightThread(LPVOID) {
    while (InterlockedCompareExchange(&g_running, 1, 1) != 0) {
        if (g_player != nullptr && g_enemy != nullptr) {
            damage(g_player, 1);
            damage(g_enemy, 1);
            // Independently, so protecting one does not resurrect the other.
            if (g_player->health < 100) {
                g_player->health = 1000;
            }
            if (g_enemy->health < 100) {
                g_enemy->health = 1000;
            }
        }
        Sleep(250);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

constexpr int idTitle = 1001;
constexpr int idText = 1002;
constexpr int idState = 1003;
constexpr int idButton1 = 1004;
constexpr int idButton2 = 1005;
constexpr int idPassword = 1006;
constexpr int idGo = 1007;
constexpr int idNext = 1008;
constexpr int idResult = 1009;

HWND g_window = nullptr;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_monoFont = nullptr;
int g_step = 0;
std::wstring g_lastState;

// Pumps messages for a while without freezing the window. Steps 6, 7 and 9 need
// real time to pass between an action and the check -- a value frozen through a
// pointer chain is rewritten by Pointer Lab every few tens of milliseconds, and
// a check that read back instantly would fail an answer that is correct.
void pumpFor(DWORD milliseconds) {
    const DWORD start = GetTickCount();
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount() - start >= milliseconds) {
            return;
        }
        Sleep(10);
    }
}

struct Step {
    const wchar_t* title;
    // The code that opens this step. Step 1 has none: it is where you start.
    const wchar_t* password;
    const wchar_t* text;
    const wchar_t* button1;
    const wchar_t* button2;
    void (*onButton1)();
    void (*onButton2)();
    // Fills `why` when it returns false. The message is the teaching, so it says
    // what was actually observed rather than "incorrect".
    bool (*check)(std::wstring& why);
    void (*state)(wchar_t* out, std::size_t count);
};

// -- Step actions -----------------------------------------------------------

void doStep2Hit() {
    step2Hit(randomBetween(1, 9));
    if (*g_step2 < 20) {
        *g_step2 = randomBetween(500, 999);
    }
}

void doStep3Hit() {
    step3Change(-randomBetween(5, 15));
    if (*g_step3 < 20) {
        *g_step3 = 20;
    }
}

void doStep3Heal() {
    step3Change(randomBetween(5, 15));
    if (*g_step3 > 1000) {
        *g_step3 = 1000;
    }
}

void doStep4Hit() {
    step4Hit(static_cast<float>(randomBetween(10, 90)) / 10.0f);
    if (*g_step4Health < 5.0f) {
        *g_step4Health = 100.0f;
    }
}

void doStep4Shoot() {
    step4Shoot(1.0);
    if (*g_step4Ammo < 1.0) {
        *g_step4Ammo = 100.0;
    }
}

void doStep5Hit() {
    step5Write(randomBetween(1, 9));
}

void doStep6Move() {
    const int carried = g_step6 != nullptr ? g_step6->health : 0;
    allocateStep6();
    if (g_step6 != nullptr) {
        g_step6->health = carried;
    }
}

void doStep6Change() {
    step6Write(randomBetween(1, 9));
}

void doStep7Move() {
    const int carried = g_step7 != nullptr ? g_step7->next->next->next->health : 0;
    allocateStep7();
    if (g_step7 != nullptr) {
        g_step7->next->next->next->health = carried;
    }
}

void doStep7Change() {
    step7Write(randomBetween(1, 9));
}

void doStep8Hit() {
    step8Hit();
}

void doStep9Reset() {
    g_player->health = 1000;
    g_enemy->health = 1000;
}

// -- Step checks ------------------------------------------------------------

bool checkStep1(std::wstring& why) {
    if (*g_step1 == 1337) {
        return true;
    }
    wchar_t buffer[256];
    swprintf_s(buffer,
               L"The value at that address is still %ld. Write 1337 to it from Pointer Lab -- "
               L"the Memory Viewer's address box, or an Address List entry.",
               *g_step1);
    why = buffer;
    return false;
}

bool checkStep2(std::wstring& why) {
    if (*g_step2 == 1000) {
        return true;
    }
    wchar_t buffer[256];
    swprintf_s(buffer, L"The value is %d, not 1000. Narrow the scan until one address is left, then "
                       L"change it.",
               *g_step2);
    why = buffer;
    return false;
}

bool checkStep3(std::wstring& why) {
    if (*g_step3 == 5000) {
        return true;
    }
    why = L"The hidden value is not 5000 yet. Alternate Hit me / Heal with Decreased value / "
          L"Increased value scans until one address survives.";
    return false;
}

bool checkStep4(std::wstring& why) {
    const bool health = *g_step4Health >= 5000.0f;
    const bool ammo = *g_step4Ammo >= 5000.0;
    if (health && ammo) {
        return true;
    }
    if (!health && !ammo) {
        why = L"Neither is at 5000 yet. Health is an f32 (float) and ammo is an f64 (double); an "
              L"i32 (int) scan will not find either of them.";
    } else if (!health) {
        why = L"Ammo is there, health is not. Health is an f32 (float) -- change the Type before "
              L"you scan for it.";
    } else {
        why = L"Health is there, ammo is not. Ammo is an f64 (double), which is eight bytes rather "
              L"than four.";
    }
    return false;
}

bool checkStep5(std::wstring& why) {
    const int before = *g_step5;
    step5Write(randomBetween(1, 9));
    const int after = *g_step5;
    if (before == after) {
        return true;
    }
    wchar_t buffer[320];
    swprintf_s(buffer,
               L"The writing code just ran and the value went from %d to %d, so the instruction is "
               L"still there. Freezing the value does not pass this step -- the check reads back "
               L"immediately. Find what writes to the address and replace that instruction with NOPs.",
               before, after);
    why = buffer;
    return false;
}

bool checkStep6(std::wstring& why) {
    if (g_step6 == nullptr) {
        why = L"Internal error: the object is missing.";
        return false;
    }
    // Moved and scrambled before reading, so an address found by scanning is
    // pointing at freed memory by the time the value is checked.
    allocateStep6();
    if (g_step6 == nullptr) {
        why = L"Internal error: the object could not be moved.";
        return false;
    }
    g_step6->health = randomBetween(1, 99);
    pumpFor(900);
    if (g_step6->health == 5000) {
        return true;
    }
    wchar_t buffer[320];
    swprintf_s(buffer,
               L"The object moved and its health is %d. A frozen plain address is now writing to "
               L"memory this program has given back. Freeze the value through the pointer chain "
               L"instead -- the entry should read module+offset rather than a bare address.",
               g_step6->health);
    why = buffer;
    return false;
}

bool checkStep7(std::wstring& why) {
    doStep7Move();
    if (g_step7 == nullptr) {
        why = L"Internal error: the chain could not be moved.";
        return false;
    }
    g_step7->next->next->next->health = randomBetween(1, 99);
    pumpFor(900);
    if (g_step7->next->next->next->health == 5000) {
        return true;
    }
    why = L"Every one of the four objects moved, and the health is not 5000. A chain that resolves "
          L"through all four hops survives this; one that stops early does not.";
    return false;
}

bool checkStep8(std::wstring& why) {
    const int before = *g_step8;
    step8Hit();
    step8Hit();
    step8Hit();
    const int after = *g_step8;
    if (after > before) {
        return true;
    }
    wchar_t buffer[320];
    swprintf_s(buffer,
               L"Hit me was called three times and the value went from %d to %d. It still subtracts. "
               L"Removing the instruction is not enough here -- the step asks for code of your own "
               L"in its place.",
               before, after);
    why = buffer;
    return false;
}

bool checkStep9(std::wstring& why) {
    const int playerBefore = g_player->health;
    const int enemyBefore = g_enemy->health;
    damage(g_player, 100);
    damage(g_enemy, 100);
    const int playerAfter = g_player->health;
    const int enemyAfter = g_enemy->health;

    const bool playerSpared = playerAfter == playerBefore;
    const bool enemyHurt = enemyAfter == enemyBefore - 100;
    if (playerSpared && enemyHurt) {
        return true;
    }
    wchar_t buffer[400];
    if (!playerSpared && !enemyHurt) {
        why = L"Neither object was written to, so the instruction is gone rather than conditional. "
              L"The enemy still has to take damage.";
    } else if (!playerSpared) {
        swprintf_s(buffer,
                   L"Your health went from %d to %d, so the write still happens for your object. "
                   L"Freezing it cannot pass this step -- the check reads back immediately.",
                   playerBefore, playerAfter);
        why = buffer;
    } else {
        why = L"Your object is protected but the enemy's is too, so the condition is not looking at "
              L"which object is being written to. The id field is at offset 4; yours is 1.";
    }
    return false;
}

// -- Live state lines -------------------------------------------------------

void stateStep1(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Step 1 value: %ld\r\nIts address this run: 0x%p", *g_step1,
               static_cast<void*>(const_cast<long*>(g_step1)));
}

void stateStep2(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Health: %d", *g_step2);
}

void stateStep3(wchar_t* out, std::size_t count) {
    // A bar and no number, which is the point: a game draws a bar. Clamped
    // because the value is about to be set to 5000 by the person reading this.
    int filled = *g_step3 * 30 / 1000;
    filled = filled < 0 ? 0 : (filled > 30 ? 30 : filled);
    wchar_t bar[40];
    for (int i = 0; i < 30; ++i) {
        bar[i] = i < filled ? L'#' : L'.';
    }
    bar[30] = L'\0';
    swprintf_s(out, count, L"Health: [%s]", bar);
}

void stateStep4(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Health: %.3f     Ammo: %.4f", static_cast<double>(*g_step4Health),
               *g_step4Ammo);
}

void stateStep5(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Health: %d", *g_step5);
}

void stateStep6(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Health: %d\r\nThe object is at 0x%p right now.",
               g_step6 != nullptr ? g_step6->health : 0, static_cast<void*>(g_step6));
}

void stateStep7(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Health: %d\r\nFour hops: 0x%p -> ... -> the value.",
               g_step7 != nullptr ? g_step7->next->next->next->health : 0,
               static_cast<void*>(g_step7));
}

void stateStep8(wchar_t* out, std::size_t count) {
    swprintf_s(out, count, L"Health: %d", *g_step8);
}

void stateStep9(wchar_t* out, std::size_t count) {
    swprintf_s(out, count,
               L"You: %d (id 1, at 0x%p)     Enemy: %d (id 2, at 0x%p)\r\n"
               L"A background thread damages both every 250 ms, through one function.",
               g_player->health, static_cast<void*>(g_player), g_enemy->health,
               static_cast<void*>(g_enemy));
}

// -- The steps themselves ---------------------------------------------------

const Step g_steps[]{
    {L"Step 1 of 9 — Attach, and write one number", nullptr,
     L"Pointer Lab talks to another program by opening a handle to it. Nothing else in this "
     L"tutorial works until that handle exists.\r\n"
     L"\r\n"
     L"1. Start Pointer Lab. In the Process Selection panel find PointerLabTutorial.exe and press "
     L"Attach. The command bar should now name this process and its width.\r\n"
     L"2. This step's value lives at the address printed below. It is printed for you exactly "
     L"once: from Step 2 on, finding the address is the work.\r\n"
     L"3. Open the Memory Viewer panel, type that address into its address box, press Go, and "
     L"change the value to 1337. Adding it to the Address List and editing it there works just as "
     L"well.\r\n"
     L"4. Press Next.\r\n"
     L"\r\n"
     L"The address is different every time this program starts, and it will be different again "
     L"tomorrow. That is ASLR, and it is the reason every later step is about finding things "
     L"rather than remembering them.",
     nullptr, nullptr, nullptr, nullptr, &checkStep1, &stateStep1},

    {L"Step 2 of 9 — Exact value scan", L"016913",
     L"The number below is real memory in this process. Find it.\r\n"
     L"\r\n"
     L"1. In the Scanner panel set Type to i32 (int) and Mode to Exact value.\r\n"
     L"2. Type the number shown below and press First scan. You will get many results: the "
     L"number is not unique, and at this point nothing distinguishes the right one.\r\n"
     L"3. Press Hit me to change it, type the new number, and press Next scan.\r\n"
     L"4. Repeat until one result is left. Double-click it to send it to the Address List, set "
     L"its value to 1000, and press Next.\r\n"
     L"\r\n"
     L"Nothing here is searched for by name. Each scan keeps only the addresses that held the old "
     L"number and now hold the new one, and two or three rounds of that eliminates every "
     L"coincidence. This is the whole of memory scanning; the rest of the tutorial is about what "
     L"to do when it is not enough.",
     L"Hit me", nullptr, &doStep2Hit, nullptr, &checkStep2, &stateStep2},

    {L"Step 3 of 9 — Unknown initial value", L"438285",
     L"This time the number is not shown. You have a health bar and nothing else, which is the "
     L"normal case: a game draws a bar, not an integer.\r\n"
     L"\r\n"
     L"1. Mode: Unknown initial value. Type: i32 (int). Press First scan. Every "
     L"four-byte value in the process is now a candidate.\r\n"
     L"2. Press Hit me. Set Mode to Decreased value and press Next scan.\r\n"
     L"3. Press Heal. Set Mode to Increased value and press Next scan.\r\n"
     L"4. Alternate until few results remain, then set the survivor to 5000 and press Next.\r\n"
     L"\r\n"
     L"Increased and Decreased compare against the previous scan rather than against a number you "
     L"typed, which is what makes a value findable when you can never see it. Unchanged value "
     L"between two presses is just as useful and often faster: most of memory does not move when "
     L"you press a button, and this is the scan that says so.",
     L"Hit me", L"Heal", &doStep3Hit, &doStep3Heal, &checkStep3, &stateStep3},

    {L"Step 4 of 9 — Float and double", L"127493",
     L"Health here is a float and ammo is a double. Neither is stored the way you would guess: an "
     L"i32 (int) scan for 100 will not find a float holding 100.0, because those bytes are "
     L"00 00 C8 42 rather than 64 00 00 00.\r\n"
     L"\r\n"
     L"1. Set Type to f32 (float) and find health exactly as you found Step 2's value, typing the "
     L"displayed number including its decimals.\r\n"
     L"2. Set Type to f64 (double) and find ammo. A double is eight bytes, and a float scan will "
     L"not find it.\r\n"
     L"3. Set both to 5000 or more and press Next.\r\n"
     L"\r\n"
     L"If an exact float scan finds nothing, that is not your mistake. A value displayed as 96.7 "
     L"is stored as 96.6999969482421875, and comparing the two bit for bit fails. Pointer Lab's "
     L"float comparison carries a tolerance for exactly this reason -- and Unknown initial value "
     L"followed by Decreased and Increased never has the problem at all, which is why it is worth "
     L"reaching for even when you can see the number.",
     L"Hit me", L"Shoot", &doStep4Hit, &doStep4Shoot, &checkStep4, &stateStep4},

    {L"Step 5 of 9 — Find out what writes to this address", L"890124",
     L"Changing a value is not the same as stopping it from changing. Here you find the "
     L"instruction doing the writing and take it out.\r\n"
     L"\r\n"
     L"1. Find the value below the usual way, pressing Hit me between scans.\r\n"
     L"2. Add it to the Address List, right-click it and choose \"Find out what writes to this "
     L"address\". The Access Watch panel opens.\r\n"
     L"3. Press Hit me. One instruction appears, with a hit count and the registers it held when "
     L"it ran.\r\n"
     L"4. Select it and press \"NOP\". The patch is padded to the next instruction "
     L"boundary, so it replaces whole instructions rather than the first half of one.\r\n"
     L"5. Press Next.\r\n"
     L"\r\n"
     L"This step checks by calling the writing code itself and reading the value back "
     L"immediately. Freezing the value cannot pass it: the freeze would not have run yet. Only "
     L"removing the instruction does.\r\n"
     L"\r\n"
     L"Afterwards, put it back from the Patches panel and watch the value start falling again. A "
     L"patch you cannot undo is a program you cannot investigate.",
     L"Hit me", nullptr, &doStep5Hit, nullptr, &checkStep5, &stateStep5},

    {L"Step 6 of 9 — A pointer", L"245367",
     L"Press \"Move it\" and the value moves to a completely different address. The address you "
     L"wrote down is now memory this program has given back to Windows, and writing to it "
     L"achieves nothing.\r\n"
     L"\r\n"
     L"Something in the program still knows where the object went. That something is a pointer, "
     L"and the pointer does not move.\r\n"
     L"\r\n"
     L"1. Find the value, using \"Change value\" to narrow the scan.\r\n"
     L"2. Add it to the Address List and run the Pointer Scanner against it.\r\n"
     L"3. Press \"Move it\", find the value's new address, and press Rescan with that address. "
     L"Only the chains that still lead to the value survive.\r\n"
     L"4. Add a surviving chain to the Address List -- it will read module+offset rather than a "
     L"bare address -- set it to 5000, and freeze it.\r\n"
     L"5. Press Next. This step moves the object again and scrambles its health before checking, "
     L"so only an entry that re-resolves its chain will still be pointing at the right place.",
     L"Move it", L"Change value", &doStep6Move, &doStep6Change, &checkStep6, &stateStep6},

    {L"Step 7 of 9 — A multi-level pointer", L"602057",
     L"The same idea, four hops deep. The value's object is reached from an object, reached from "
     L"an object, reached from an object, reached from a static address -- and Move it "
     L"reallocates every one of them.\r\n"
     L"\r\n"
     L"1. Find the value and run the Pointer Scanner against it, allowing at least five levels.\r\n"
     L"2. Press \"Move it\", find the new address, and Rescan. One pass usually leaves a handful "
     L"of chains; two leaves the right one.\r\n"
     L"3. Freeze the value at 5000 through the chain and press Next.\r\n"
     L"\r\n"
     L"A long chain is not harder than a short one, only longer. What matters is that the chain "
     L"is rooted in something that does not move -- an address inside a module's own image -- "
     L"because everything downstream of it is allocated somewhere new on every run.",
     L"Move it", L"Change value", &doStep7Move, &doStep7Change, &checkStep7, &stateStep7},

    {L"Step 8 of 9 — Code injection", L"705021",
     L"Removing an instruction is blunt. Replacing it with code of your own is the general "
     L"tool.\r\n"
     L"\r\n"
     L"Here Hit me subtracts 1. Make it add instead.\r\n"
     L"\r\n"
     L"1. Find the value, then find out what writes to it.\r\n"
     L"2. On that row press Script and choose \"Full injection (keeps the original "
     L"instruction)\". The Scripts panel opens with a skeleton: an aobscanmodule that finds the "
     L"instruction by its bytes rather than by an address that will not survive a restart, an "
     L"alloc near enough for a five-byte jmp to reach it, and the original instruction copied "
     L"into the cave.\r\n"
     L"3. Change the copied instruction so it adds rather than subtracts.\r\n"
     L"4. Press Check first. It runs the whole script, resolves every symbol and computes the "
     L"layout, and writes and allocates nothing -- so you can read what it worked out before it "
     L"touches the program.\r\n"
     L"5. Enable it and press Next. This step calls the writing code three times and passes if "
     L"the value went up.\r\n"
     L"\r\n"
     L"An injection written down is worth more than the same edit made by hand. It can be read "
     L"before it runs, kept, and run again tomorrow after every address has changed.",
     L"Hit me", nullptr, &doStep8Hit, nullptr, &checkStep8, &stateStep8},

    {L"Step 9 of 9 — Shared code", L"441398",
     L"One function writes both healths -- yours and the enemy's -- from one instruction, called "
     L"from a background thread. NOP it and neither of you can be hurt, which is not a cheat, it "
     L"is a broken game.\r\n"
     L"\r\n"
     L"The answer is to inject code that looks at which object is being written to and steps over "
     L"the write for exactly one of them.\r\n"
     L"\r\n"
     L"1. Find your health -- you are id 1 -- and find out what writes to it. One instruction "
     L"appears, with a hit count climbing on its own, because the thread never stops.\r\n"
     L"2. Look at the registers captured on that hit. One of them holds the address of the object "
     L"being damaged. The id field is at offset 4 in that object.\r\n"
     L"3. Write a full injection that compares that id against 1 and jumps over the write when it "
     L"matches, and enable it.\r\n"
     L"4. Press Next. This step damages both objects directly, once each, and reads back "
     L"immediately: it passes only if yours did not change and the enemy's dropped by 100. "
     L"Freezing your health cannot pass it, and neither can removing the instruction.\r\n"
     L"\r\n"
     L"This is the step that separates changing a number from changing a program. Everything up "
     L"to here found a value; this one had to understand what the code was doing with it.",
     L"Restart the fight", nullptr, &doStep9Reset, nullptr, &checkStep9, &stateStep9},
};

constexpr int stepCount = static_cast<int>(sizeof(g_steps) / sizeof(g_steps[0]));

// ---------------------------------------------------------------------------
// Painting the current step onto the controls
// ---------------------------------------------------------------------------

void setResult(const std::wstring& text) {
    SetDlgItemTextW(g_window, idResult, text.c_str());
}

void showStep() {
    const Step& step = g_steps[g_step];
    SetDlgItemTextW(g_window, idTitle, step.title);
    SetDlgItemTextW(g_window, idText, step.text);

    HWND button1 = GetDlgItem(g_window, idButton1);
    HWND button2 = GetDlgItem(g_window, idButton2);
    if (step.button1 != nullptr) {
        SetWindowTextW(button1, step.button1);
    }
    ShowWindow(button1, step.button1 != nullptr ? SW_SHOW : SW_HIDE);
    if (step.button2 != nullptr) {
        SetWindowTextW(button2, step.button2);
    }
    ShowWindow(button2, step.button2 != nullptr ? SW_SHOW : SW_HIDE);

    SetDlgItemTextW(g_window, idNext, g_step + 1 < stepCount ? L"Next" : L"Finish");
    g_lastState.clear();
}

void updateState() {
    wchar_t buffer[512];
    buffer[0] = L'\0';
    g_steps[g_step].state(buffer, sizeof(buffer) / sizeof(buffer[0]));
    // Only when it changed, or the static flickers thirty times a second and
    // the number underneath becomes hard to read.
    if (g_lastState != buffer) {
        g_lastState = buffer;
        SetDlgItemTextW(g_window, idState, buffer);
    }
}

void advance() {
    if (g_step + 1 >= stepCount) {
        setResult(L"That is all nine. Every technique in the book's Part II is now something you "
                  L"have done rather than something you have read: exact and unknown scans, "
                  L"typed values, find-what-writes, single and multi-level pointers, and two "
                  L"kinds of code injection. Close this and go and find something of your own.");
        EnableWindow(GetDlgItem(g_window, idNext), FALSE);
        return;
    }
    ++g_step;
    showStep();
    std::wstring message = L"Correct. The password for this step is ";
    message += g_steps[g_step].password;
    message += L" -- write it down, and you can come straight back here next time rather than "
               L"working through everything before it.";
    setResult(message);
}

void onNext() {
    std::wstring why;
    if (g_steps[g_step].check(why)) {
        advance();
        return;
    }
    setResult(why);
}

void onGoToStep() {
    wchar_t typed[64]{};
    GetDlgItemTextW(g_window, idPassword, typed, static_cast<int>(sizeof(typed) / sizeof(typed[0])));
    for (int i = 1; i < stepCount; ++i) {
        if (g_steps[i].password != nullptr && std::wcscmp(typed, g_steps[i].password) == 0) {
            g_step = i;
            showStep();
            setResult(L"Jumped to this step. The earlier ones are not checked -- the password is "
                      L"the proof that you did them.");
            SetDlgItemTextW(g_window, idPassword, L"");
            return;
        }
    }
    setResult(L"That is not a step password. Each one is shown when you pass the step before it.");
}

HWND makeControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width,
                 int height, int id, HFONT font) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width,
                                   height, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return control;
}

void createControls() {
    makeControl(L"STATIC", L"", 0, 16, 12, 748, 26, idTitle, g_titleFont);
    makeControl(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 16, 46, 748, 300,
                idText, g_font);
    makeControl(L"STATIC", L"", SS_LEFT, 16, 356, 748, 40, idState, g_monoFont);
    makeControl(L"BUTTON", L"", BS_PUSHBUTTON, 16, 402, 150, 30, idButton1, g_font);
    makeControl(L"BUTTON", L"", BS_PUSHBUTTON, 176, 402, 150, 30, idButton2, g_font);

    makeControl(L"STATIC", L"Step password:", SS_LEFT, 16, 446, 100, 22, 0, g_font);
    makeControl(L"EDIT", L"", WS_BORDER, 120, 442, 110, 26, idPassword, g_font);
    makeControl(L"BUTTON", L"Go to step", BS_PUSHBUTTON, 240, 442, 110, 26, idGo, g_font);
    makeControl(L"BUTTON", L"Next", BS_DEFPUSHBUTTON, 654, 440, 110, 30, idNext, g_font);

    makeControl(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 16, 482, 748, 82,
                idResult, g_font);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        createControls();
        showStep();
        setResult(L"Welcome. Work through the steps in order; each one tells you the password for "
                  L"the next, so you never have to repeat what you have already done.");
        SetTimer(window, 1, 100, nullptr);
        return 0;

    case WM_TIMER:
        updateState();
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == idNext) {
            onNext();
        } else if (id == idGo) {
            onGoToStep();
        } else if (id == idButton1 && g_steps[g_step].onButton1 != nullptr) {
            g_steps[g_step].onButton1();
            updateState();
        } else if (id == idButton2 && g_steps[g_step].onButton2 != nullptr) {
            g_steps[g_step].onButton2();
            updateState();
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        // Read-only edits send this too, which is wanted: the instruction and
        // result boxes should read as panels rather than as fields to type in.
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_DESTROY:
        KillTimer(window, 1);
        InterlockedExchange(&g_running, 0);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    std::srand(GetTickCount());
    if (!allocateEverything()) {
        MessageBoxW(nullptr, L"Could not allocate the tutorial's memory.", L"Pointer Lab Tutorial",
                    MB_ICONERROR);
        return 1;
    }

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    g_titleFont = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    g_monoFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = L"PointerLabTutorialWindow";
    if (RegisterClassExW(&windowClass) == 0) {
        return 1;
    }

    RECT wanted{0, 0, 780, 580};
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    AdjustWindowRect(&wanted, style, FALSE);

    const int bits = static_cast<int>(sizeof(void*) * 8);
    wchar_t caption[64];
    swprintf_s(caption, L"Pointer Lab Tutorial (%d-bit)", bits);

    HWND window = CreateWindowExW(0, windowClass.lpszClassName, caption, style, CW_USEDEFAULT,
                                  CW_USEDEFAULT, wanted.right - wanted.left,
                                  wanted.bottom - wanted.top, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    HANDLE thread = CreateThread(nullptr, 0, &fightThread, nullptr, 0, nullptr);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message) == 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    InterlockedExchange(&g_running, 0);
    if (thread != nullptr) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    return static_cast<int>(message.wParam);
}
