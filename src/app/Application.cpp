#include "app/Application.h"

#include "infra/CrashHandler.h"
#include "infra/Logger.h"
#include "infra/Paths.h"
#include "ui/UiApp.h"

#include <filesystem>

namespace ire::app {

int Application::run(HINSTANCE instance, int showCommand) {
    std::error_code ignored;
    std::filesystem::create_directories(infra::Paths::appData(), ignored);
    infra::Logger::instance().initialize(infra::Paths::logFile());
    infra::CrashHandler::install();
    infra::Logger::instance().info("Pointer Lab starting.");

    ui::UiApp ui(instance, showCommand);
    return ui.run();
}

} // namespace ire::app

