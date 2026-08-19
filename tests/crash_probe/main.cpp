// A process whose only job is to install the crash handler and then die in a
// specific way, so the crash path can be tested. Every other part of the
// application can be exercised in-process; this one cannot, because a
// successful run ends with the process gone.
//
// Usage: pointerlab_crash_probe <mode> <appdata-directory>
//   mode = access-violation | throw | invalid-parameter

#include "infra/CrashHandler.h"

#include <Windows.h>

#include <cstdio>
#include <cwchar>
#include <stdexcept>

namespace {

// volatile, and at file scope, so the compiler has to emit a real load and a
// real store to address zero. A local null constant would let it fold the
// dereference into an illegal-instruction trap instead, which raises a
// different exception code than the access violation being tested.
volatile int* g_nothing = nullptr;
FILE* volatile g_noStream = nullptr;

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        return 2;
    }

    // Paths::appData() reads LOCALAPPDATA, so redirecting it keeps the probe's
    // dumps out of the real Pointer Lab folder and gives the test a directory
    // it can inspect and delete.
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", argv[2])) {
        return 3;
    }

    ire::infra::CrashHandler::install(/*interactive=*/false);

    if (std::wcscmp(argv[1], L"access-violation") == 0) {
        *g_nothing = 1;
    } else if (std::wcscmp(argv[1], L"throw") == 0) {
        throw std::runtime_error("deliberate uncaught exception");
    } else if (std::wcscmp(argv[1], L"invalid-parameter") == 0) {
        // fclose on a null stream is documented to invoke the invalid
        // parameter handler rather than raising an exception, so this reaches
        // the handler by a route the other two modes never touch.
        std::fclose(g_noStream);
    } else {
        return 4;
    }

    // Reaching here means the crash did not happen, which is itself a failure.
    return 5;
}
