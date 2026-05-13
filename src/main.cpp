#include "app/Application.h"

#include <Windows.h>

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    ire::app::Application app;
    return app.run(instance, showCommand);
}

