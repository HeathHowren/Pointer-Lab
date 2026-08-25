#include "infra/CrashHandler.h"

#include "infra/Logger.h"
#include "infra/Paths.h"

#include <Version.h>

#include <Windows.h>
// DbgHelp must follow Windows.h.
#include <DbgHelp.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>

namespace ire::infra {

namespace {

// Everything the crash path needs is prepared up front. A process that has just
// faulted may have a corrupt heap, so allocating, formatting or taking a lock
// inside the filter can turn a recoverable report into a silent second crash.
wchar_t g_dumpPath[MAX_PATH]{};
wchar_t g_logPath[MAX_PATH]{};
wchar_t g_enginePath[MAX_PATH]{};
wchar_t g_message[2048]{};
std::atomic<bool> g_handling{false};
std::atomic<bool> g_interactive{true};

void copyPath(wchar_t (&destination)[MAX_PATH], const std::filesystem::path& path) {
    const auto text = path.wstring();
    const std::size_t count = text.size() < MAX_PATH ? text.size() : MAX_PATH - 1;
    std::wmemcpy(destination, text.c_str(), count);
    destination[count] = L'\0';
}

bool writeDumpOfType(EXCEPTION_POINTERS* exceptionInfo, MINIDUMP_TYPE type) {
    HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = exceptionInfo;
    information.ClientPointers = FALSE;

    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                           exceptionInfo != nullptr ? &information : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return written != FALSE;
}

bool writeMinidump(EXCEPTION_POINTERS* exceptionInfo) {
    if (g_dumpPath[0] == L'\0') {
        return false;
    }

    // Enough to get a usable stack and the values around it without writing out
    // the entire address space, which for this application can be very large.
    const auto rich = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithDataSegs |
                                                 MiniDumpWithHandleData |
                                                 MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules);
    if (writeDumpOfType(exceptionInfo, rich)) {
        return true;
    }

    // MiniDumpWithIndirectlyReferencedMemory walks everything the stack points
    // at, and that walk is the part that fails: it can come back false after
    // having already written most of the file, which is how a crash could leave
    // behind a large .dmp and a log line saying no dump was written. It is
    // worst on the terminate path, where an exception is still in flight while
    // the walk runs. A plain dump needs no walk, so start the file again as one
    // rather than keep a partial dump nobody is told to send. A stack-only dump
    // is much less than the full one and much more than nothing.
    return writeDumpOfType(exceptionInfo, MiniDumpNormal);
}

// Deliberately uses the raw Win32 file API rather than the Logger: the Logger
// takes a mutex, and the crashing thread may already hold it.
//
// Opening the crash log is the one step here that touches a file the rest of the
// machine can see too. Real-time antivirus scans a file when it is closed, and
// that scan holds it open; an open landing inside the window comes back
// ERROR_SHARING_VIOLATION. So retry briefly, and share widely enough that a
// reader can never be the reason a crash report is lost.
HANDLE openCrashLog() {
    if (g_logPath[0] == L'\0') {
        return INVALID_HANDLE_VALUE;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        const HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION) {
            return INVALID_HANDLE_VALUE;
        }
        Sleep(25);
    }
    return INVALID_HANDLE_VALUE;
}

// Returns false when the line did not reach the file, so the caller can stop
// telling the user to attach a log that was never written.
bool appendCrashLine(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const auto length = static_cast<DWORD>(std::strlen(text));
    DWORD written{};
    return WriteFile(file, text, length, &written, nullptr) != FALSE && written == length;
}

void report(const wchar_t* what, EXCEPTION_POINTERS* exceptionInfo) {
    // A fault inside the handler must not recurse forever.
    bool expected = false;
    if (!g_handling.compare_exchange_strong(expected, true)) {
        return;
    }

    char line[512]{};
    if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr) {
        std::snprintf(line, sizeof(line),
                      "\r\n--- Pointer Lab " POINTERLAB_VERSION_STRING
                      " crash: code 0x%08lX at 0x%p (thread %lu) ---\r\n",
                      exceptionInfo->ExceptionRecord->ExceptionCode,
                      exceptionInfo->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line),
                      "\r\n--- Pointer Lab " POINTERLAB_VERSION_STRING
                      " crash (no exception record, thread %lu) ---\r\n",
                      GetCurrentThreadId());
    }
    // One handle, held open across the minidump write. The previous code opened
    // the log twice, and the second open sat immediately after a freshly closed
    // .dmp -- precisely when an antivirus scan of that .dmp is most likely to be
    // touching the folder. That open could fail, the failure was discarded, and
    // the crash line vanished with no trace that anything had gone wrong.
    const HANDLE log = openCrashLog();
    bool logged = appendCrashLine(log, line);

    const bool dumped = writeMinidump(exceptionInfo);
    logged = appendCrashLine(log, dumped ? "minidump written\r\n" : "minidump could NOT be written\r\n") && logged;

    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }

    if (!g_interactive.load()) {
        return;
    }

    // Telling the user where the dump landed is the whole point; a crash that
    // vanishes without a trace is one nobody can ever report.
    _snwprintf_s(g_message, _TRUNCATE,
                 L"Pointer Lab " POINTERLAB_VERSION_WIDE L" has to close.\n\n%ls\n\n"
                 L"%ls\n%ls\n%ls\n\n"
                 L"Please attach these files if you report this.",
                 what,
                 dumped ? g_dumpPath : L"(a crash dump could not be written)",
                 logged ? g_logPath : L"(the crash log could not be written)",
                 g_enginePath);
    MessageBoxW(nullptr, g_message, L"Pointer Lab crashed", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

LONG WINAPI handleUnhandledException(EXCEPTION_POINTERS* exceptionInfo) {
    report(L"An unhandled exception was raised.", exceptionInfo);
    return EXCEPTION_EXECUTE_HANDLER;
}

// An uncaught C++ exception unwinds through std::terminate and never reaches
// the SEH filter, so without this a throw from anywhere produced no dump and no
// message at all.
void handleTerminate() {
    report(L"A C++ exception was thrown and never caught.", nullptr);
    _exit(3);
}

void handleInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    report(L"A C runtime function was called with an invalid parameter.", nullptr);
    _exit(3);
}

void handlePureCall() {
    report(L"A pure virtual function was called.", nullptr);
    _exit(3);
}

} // namespace

void CrashHandler::install(bool interactive) {
    g_interactive = interactive;
    copyPath(g_dumpPath, Paths::crashDumpFile());
    copyPath(g_logPath, Paths::crashFile());
    copyPath(g_enginePath, Paths::logFile());

    std::error_code ignored;
    std::filesystem::create_directories(Paths::appData(), ignored);

    SetUnhandledExceptionFilter(handleUnhandledException);
    std::set_terminate(&handleTerminate);
    _set_invalid_parameter_handler(&handleInvalidParameter);
    _set_purecall_handler(&handlePureCall);

    // Otherwise the CRT pops its own "abnormal program termination" box first
    // and the process dies before the filter ever runs.
    _CrtSetReportMode(_CRT_ASSERT, 0);
}

bool CrashHandler::writeDumpNow() {
    return writeMinidump(nullptr);
}

} // namespace ire::infra
