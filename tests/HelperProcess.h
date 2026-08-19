#pragma once

// Spawns tests/helper/main.cpp as a real target process and talks to it over
// pipes. Scanning, reading, writing and disassembling all behave differently
// across a process boundary than they do inside the test process, so the
// integration tests use this rather than targeting themselves.

#include <catch2/catch_test_macros.hpp>

#include "domain/TargetSession.h"
#include "platform_win32/Win32Platform.h"

#include <Windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace testsupport {

// The value tests/helper/main.cpp starts out holding.
inline constexpr std::int32_t needleValue = 0x5AFE1234;

inline std::filesystem::path helperPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path() / L"pointerlab_test_helper.exe";
}

class HelperProcess {
public:
    HelperProcess() {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;

        HANDLE childStdIn{};
        HANDLE childStdOut{};
        if (!CreatePipe(&childStdIn, &toChild_, &attributes, 0)) {
            return;
        }
        if (!CreatePipe(&fromChild_, &childStdOut, &attributes, 0)) {
            CloseHandle(childStdIn);
            return;
        }
        // The parent's own ends must not be inherited, or the pipes never
        // report EOF once the child exits.
        SetHandleInformation(toChild_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(fromChild_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childStdIn;
        startup.hStdOutput = childStdOut;
        startup.hStdError = childStdOut;

        std::wstring command = L"\"" + helperPath().wstring() + L"\"";
        PROCESS_INFORMATION information{};
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, nullptr, &startup, &information);

        CloseHandle(childStdIn);
        CloseHandle(childStdOut);
        if (!created) {
            return;
        }

        process_ = information.hProcess;
        pid_ = information.dwProcessId;
        CloseHandle(information.hThread);

        address_ = parseHexReply(readLine(), "ADDR ");
        root_ = parseHexReply(readLine(), "ROOT ");
        tick_ = parseHexReply(readLine(), "TICK ");
    }

    ~HelperProcess() {
        if (process_ != nullptr) {
            send("QUIT");
            if (WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) {
                TerminateProcess(process_, 1);
            }
            CloseHandle(process_);
        }
        if (toChild_ != nullptr) {
            CloseHandle(toChild_);
        }
        if (fromChild_ != nullptr) {
            CloseHandle(fromChild_);
        }
    }

    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;

    [[nodiscard]] bool ready() const { return process_ != nullptr && address_ != 0 && tick_ != 0; }
    [[nodiscard]] std::uint32_t pid() const { return pid_; }
    [[nodiscard]] std::uintptr_t address() const { return address_; }
    [[nodiscard]] std::uintptr_t root() const { return root_; }
    // Entry point of tick(), which a worker thread calls continuously.
    [[nodiscard]] std::uintptr_t tick() const { return tick_; }

    // A page-sized scratch area past the value, for tests that need somewhere
    // in the target to write bytes without disturbing the needle.
    [[nodiscard]] std::uintptr_t scratch() const { return address_ + 256; }

    // Blocks until the helper acknowledges, so the value is guaranteed to have
    // changed before the next scan starts.
    bool set(std::int32_t value) {
        if (!send("SET " + std::to_string(value))) {
            return false;
        }
        return readLine().rfind("OK ", 0) == 0;
    }

    std::int32_t get() {
        if (!send("GET")) {
            return 0;
        }
        const auto line = readLine();
        if (line.rfind("VAL ", 0) != 0) {
            return 0;
        }
        return static_cast<std::int32_t>(std::strtol(line.c_str() + 4, nullptr, 10));
    }

    // How many times the worker thread has called tick(). Answering means the
    // helper's main thread is still responsive, so this doubles as a liveness
    // check: -1 means the process never replied.
    std::int64_t ticks() {
        if (!send("TICKS")) {
            return -1;
        }
        const auto line = readLine();
        if (line.rfind("TICKCOUNT ", 0) != 0) {
            return -1;
        }
        return static_cast<std::int64_t>(std::strtoll(line.c_str() + 10, nullptr, 10));
    }

private:
    static std::uintptr_t parseHexReply(const std::string& line, const char* prefix) {
        if (line.rfind(prefix, 0) != 0) {
            return 0;
        }
        return static_cast<std::uintptr_t>(std::strtoull(line.c_str() + std::strlen(prefix), nullptr, 16));
    }

    bool send(const std::string& command) {
        if (toChild_ == nullptr) {
            return false;
        }
        const std::string line = command + "\n";
        DWORD written{};
        return WriteFile(toChild_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE &&
               written == line.size();
    }

    std::string readLine() {
        std::string line;
        char ch{};
        DWORD read{};
        while (ReadFile(fromChild_, &ch, 1, &read, nullptr) && read == 1) {
            if (ch == '\n') {
                break;
            }
            if (ch != '\r') {
                line.push_back(ch);
            }
        }
        return line;
    }

    HANDLE toChild_{};
    HANDLE fromChild_{};
    HANDLE process_{};
    std::uint32_t pid_{};
    std::uintptr_t address_{};
    std::uintptr_t root_{};
    std::uintptr_t tick_{};
};

// A spawned helper with a session already attached to it.
struct AttachedHelper {
    HelperProcess helper;
    ire::platform_win32::Win32Platform platform;
    ire::domain::TargetSession session{platform};

    AttachedHelper() {
        REQUIRE(helper.ready());
        REQUIRE(session.attach(helper.pid()).has_value());
        REQUIRE(session.attached());
    }
};

} // namespace testsupport
