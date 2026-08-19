#pragma once

namespace ire::infra {

class CrashHandler {
public:
    // Installs the unhandled-exception filter plus the terminate,
    // invalid-parameter and pure-call handlers, so a C++ throw produces a dump
    // as reliably as an access violation does.
    //
    // interactive=false suppresses the message box so the crash path can be
    // exercised without a human there to dismiss it; the dump and the log entry
    // are still written.
    static void install(bool interactive = true);

    // Writes a dump of the current state without crashing. Returns false if the
    // dump could not be written.
    static bool writeDumpNow();
};

} // namespace ire::infra

