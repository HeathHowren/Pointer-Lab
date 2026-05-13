#pragma once

#include <Windows.h>

namespace ire::app {

class Application {
public:
    int run(HINSTANCE instance, int showCommand);
};

} // namespace ire::app

