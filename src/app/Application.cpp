#include "app/Application.h"

#include "infra/CrashHandler.h"
#include "infra/Logger.h"
#include "infra/Paths.h"
#include "platform_win32/Win32Platform.h"
#include "ui/UiApp.h"

#include <Version.h>

#include <filesystem>

namespace ire::app {

int Application::run(HINSTANCE instance, int showCommand) {
    std::error_code ignored;
    std::filesystem::create_directories(infra::Paths::appData(), ignored);
    infra::Logger::instance().initialize(infra::Paths::logFile());
    infra::CrashHandler::install();
    infra::Logger::instance().info("Pointer Lab " POINTERLAB_VERSION_STRING " starting.");

    // Without SeDebugPrivilege many processes cannot be opened for writing at
    // all. Not being able to acquire it is normal when running unelevated, so
    // this is a note rather than a failure.
    if (auto privilege = platform_win32::Win32Platform::enableDebugPrivilege()) {
        infra::Logger::instance().info("SeDebugPrivilege enabled.");
    } else {
        infra::Logger::instance().warn(privilege.error());
    }

    ui::UiApp ui(instance, showCommand);
    return ui.run();
}

} // namespace ire::app

