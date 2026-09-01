#include "app/Application.h"

#include "infra/CrashHandler.h"
#include "infra/Logger.h"
#include "infra/Paths.h"
#include "platform_win32/Win32Platform.h"
#include "ui/UiApp.h"

#include <Version.h>

#include <shellapi.h>

#include <filesystem>
#include <string>

namespace ire::app {

namespace {

// The command line, which is two options long.
//
// `--script <file>` runs a Lua file once the window is up. It exists for
// scripted figure capture: a set of screenshots produced by running a committed
// file is re-produced identically on every release, and a panel that has been
// renamed since fails the run rather than quietly disagreeing with a caption.
//
// Deliberately not a general batch mode. The script runs in the ordinary
// console, with the ordinary sandbox, against the ordinary window; the flag only
// saves someone from pasting it in.
//
// `--mcp [port]` starts the MCP server as the window comes up, for someone who
// runs Pointer Lab in order to hand it to an agent and would otherwise open the
// panel and press Start every time. It is not a way to run without a window: the
// token still has to be read off the panel, which is the point -- the flag saves
// a click, not the decision.
struct CommandLine {
    std::filesystem::path script;
    bool mcp{};
    // 0 means "let Windows choose", which is also what the panel's 0 means.
    std::uint16_t mcpPort{8722};
    bool help{};
    std::wstring unknown;
};

CommandLine parseCommandLine() {
    CommandLine parsed;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return parsed;
    }
    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        if (argument == L"--script" && i + 1 < count) {
            parsed.script = arguments[++i];
        } else if (argument == L"--mcp") {
            parsed.mcp = true;
            // The port is optional and follows only if it looks like one, so
            // `--mcp --script x.lua` does not eat the next flag as a port.
            if (i + 1 < count) {
                const std::wstring next = arguments[i + 1];
                if (!next.empty() && next.find_first_not_of(L"0123456789") == std::wstring::npos) {
                    const auto port = std::wcstoul(next.c_str(), nullptr, 10);
                    if (port <= 65535) {
                        parsed.mcpPort = static_cast<std::uint16_t>(port);
                        ++i;
                    }
                }
            }
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            parsed.help = true;
        } else if (parsed.unknown.empty()) {
            parsed.unknown = argument;
        }
    }
    LocalFree(arguments);
    return parsed;
}

} // namespace

int Application::run(HINSTANCE instance, int showCommand) {
    const CommandLine command = parseCommandLine();
    if (command.help) {
        MessageBoxW(nullptr,
                    L"Pointer Lab " POINTERLAB_VERSION_STRING L"\n\n"
                    L"PointerLab.exe [--script <file.lua>] [--mcp [port]]\n\n"
                    L"--script runs a Lua script once the window is up. It is the same language "
                    L"and the same sandbox as the Lua Console, plus screenshot(), select_panel(), "
                    L"set_layout(), set_window_size(), wait_frames() and quit(), and it exists so "
                    L"that a set of figures can be captured by running a file.\n\n"
                    L"--mcp starts the MCP server on 127.0.0.1, so an AI agent can drive this "
                    L"session. The port defaults to 8722; 0 lets Windows choose. The session token "
                    L"is shown in the MCP Server panel and is needed by any client.\n\n"
                    L"While that server is running, whatever holds the token can read and write the "
                    L"target's memory, patch its code and inject into it without confirming each "
                    L"action.",
                    L"Pointer Lab", MB_ICONINFORMATION);
        return 0;
    }

    std::error_code ignored;
    std::filesystem::create_directories(infra::Paths::appData(), ignored);
    infra::Logger::instance().initialize(infra::Paths::logFile());
    infra::CrashHandler::install();
    infra::Logger::instance().info("Pointer Lab " POINTERLAB_VERSION_STRING " starting.");
    if (!command.unknown.empty()) {
        infra::Logger::instance().warn("Ignoring an unrecognised command-line argument.");
    }

    // Without SeDebugPrivilege many processes cannot be opened for writing at
    // all. Not being able to acquire it is normal when running unelevated, so
    // this is a note rather than a failure.
    if (auto privilege = platform_win32::Win32Platform::enableDebugPrivilege()) {
        infra::Logger::instance().info("SeDebugPrivilege enabled.");
    } else {
        infra::Logger::instance().warn(privilege.error());
    }

    ui::UiApp ui(instance, showCommand);
    if (!command.script.empty()) {
        ui.setStartupScript(command.script);
    }
    if (command.mcp) {
        ui.setStartupMcpPort(command.mcpPort);
    }
    return ui.run();
}

} // namespace ire::app
