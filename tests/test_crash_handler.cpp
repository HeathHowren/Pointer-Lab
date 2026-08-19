// Exercises the crash handler by crashing a child process on purpose and
// inspecting what it left behind. The handler is the one piece of the
// application that cannot be tested in-process, and it is also the piece that
// is never noticed as broken until someone actually needs it.

#include <catch2/catch_test_macros.hpp>

#include "infra/Paths.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path probePath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path() / L"pointerlab_crash_probe.exe";
}

// A directory the probe is told to treat as LOCALAPPDATA, so its dumps land
// somewhere the test can inspect and then delete.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& name) {
        std::error_code ignored;
        path_ = std::filesystem::temp_directory_path(ignored) / ("pointerlab-crash-" + name);
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path dump() const { return path_ / "PointerLab" / "crash.dmp"; }
    [[nodiscard]] std::filesystem::path log() const { return path_ / "PointerLab" / "crash.log"; }

private:
    std::filesystem::path path_;
};

// Returns the probe's exit code, or -1 if it could not be run or did not finish.
int runProbe(const std::wstring& mode, const std::filesystem::path& appData) {
    const auto exe = probePath();
    if (exe.empty() || !std::filesystem::exists(exe)) {
        return -1;
    }

    std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + mode + L" \"" + appData.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    // CREATE_NO_WINDOW keeps the console probe from flashing a window during a
    // test run; the probe never writes to its console anyway.
    if (!CreateProcessW(exe.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process)) {
        return -1;
    }

    // Writing a minidump of a process this small is quick; the generous timeout
    // is only there so a stuck probe fails the test instead of hanging CI.
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = static_cast<DWORD>(-1);
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    } else {
        TerminateProcess(process.hProcess, 1);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return waited == WAIT_OBJECT_0 ? static_cast<int>(exitCode) : -1;
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("An access violation produces a minidump and a crash log entry", "[crash]") {
    ScratchDirectory scratch("av");
    const int exitCode = runProbe(L"access-violation", scratch.path());

    // Exit code 5 means the probe reached the end without crashing, which would
    // mean the test proved nothing.
    REQUIRE(exitCode != 5);
    REQUIRE(exitCode != -1);

    REQUIRE(std::filesystem::exists(scratch.dump()));
    // A minidump with real thread and memory data is never this small; an empty
    // or header-only file would mean MiniDumpWriteDump failed quietly.
    CHECK(std::filesystem::file_size(scratch.dump()) > 4096);

    REQUIRE(std::filesystem::exists(scratch.log()));
    const auto log = readAll(scratch.log());
    CHECK(log.find("crash: code 0xC0000005") != std::string::npos);
    CHECK(log.find("minidump written") != std::string::npos);
}

TEST_CASE("An uncaught C++ exception produces a minidump", "[crash]") {
    // An uncaught throw unwinds through std::terminate and never reaches the
    // SEH filter, so this covers a completely separate code path from the
    // access violation above.
    ScratchDirectory scratch("throw");
    const int exitCode = runProbe(L"throw", scratch.path());

    REQUIRE(exitCode != 5);
    REQUIRE(exitCode != -1);

    REQUIRE(std::filesystem::exists(scratch.dump()));
    CHECK(std::filesystem::file_size(scratch.dump()) > 4096);
    CHECK(readAll(scratch.log()).find("minidump written") != std::string::npos);
}

TEST_CASE("An invalid CRT parameter produces a minidump", "[crash]") {
    ScratchDirectory scratch("invalid-parameter");
    const int exitCode = runProbe(L"invalid-parameter", scratch.path());

    REQUIRE(exitCode != 5);
    REQUIRE(exitCode != -1);

    REQUIRE(std::filesystem::exists(scratch.dump()));
    CHECK(readAll(scratch.log()).find("minidump written") != std::string::npos);
}

TEST_CASE("The crash paths all live in one folder", "[crash]") {
    // The crash message tells the user to send these three files; if they ever
    // drift apart the instruction becomes wrong.
    const auto folder = ire::infra::Paths::appData();
    CHECK(ire::infra::Paths::crashDumpFile().parent_path() == folder);
    CHECK(ire::infra::Paths::crashFile().parent_path() == folder);
    CHECK(ire::infra::Paths::logFile().parent_path() == folder);
}
