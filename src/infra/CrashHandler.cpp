#include "infra/CrashHandler.h"

#include "infra/Logger.h"
#include "infra/Paths.h"

#include <Windows.h>

#include <fstream>
#include <sstream>

namespace ire::infra {

namespace {

LONG WINAPI handleUnhandledException(EXCEPTION_POINTERS* exceptionInfo) {
    std::ofstream out(Paths::crashFile(), std::ios::app);
    out << "Unhandled exception\n";
    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        out << "Code: 0x" << std::hex << exceptionInfo->ExceptionRecord->ExceptionCode << "\n";
        out << "Address: 0x" << reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress) << "\n";
    }
    Logger::instance().error("Unhandled exception captured. See crash.log.");
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void CrashHandler::install() {
    SetUnhandledExceptionFilter(handleUnhandledException);
}

} // namespace ire::infra

