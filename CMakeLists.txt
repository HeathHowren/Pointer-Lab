cmake_minimum_required(VERSION 3.24)

project(PointerLab VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
)

FetchContent_MakeAvailable(imgui lua)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

file(GLOB LUA_CORE CONFIGURE_DEPENDS ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM LUA_CORE
    ${lua_SOURCE_DIR}/src/lua.c
    ${lua_SOURCE_DIR}/src/luac.c
    ${lua_SOURCE_DIR}/src/onelua.c
)
add_library(lua54 STATIC ${LUA_CORE})
target_include_directories(lua54 PUBLIC ${lua_SOURCE_DIR}/src)

add_executable(PointerLab WIN32
    src/main.cpp
    src/app/Application.cpp
    src/domain/Domain.cpp
    src/domain/AddressList.cpp
    src/domain/TargetSession.cpp
    src/engine_asm/Assembler.cpp
    src/engine_disasm/Disassembler.cpp
    src/engine_inject/Injector.cpp
    src/engine_pointer/PointerScanner.cpp
    src/engine_scan/MemoryScanner.cpp
    src/infra/CrashHandler.cpp
    src/infra/Logger.cpp
    src/infra/Paths.cpp
    src/platform_win32/Win32Platform.cpp
    src/scripting/LuaConsole.cpp
    src/scripting/LuaScanner.cpp
    src/services/RuntimeServices.cpp
    src/storage/ProjectStore.cpp
    src/ui/UiApp.cpp
)

target_include_directories(PointerLab PRIVATE src)
target_compile_definitions(PointerLab PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(PointerLab PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

target_link_libraries(PointerLab PRIVATE
    imgui
    lua54
    d3d11
    dxgi
    dwmapi
    comctl32
    psapi
    dbghelp
)

add_custom_command(TARGET PointerLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PointerLab>/resources/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf"
        "${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf"
        "$<TARGET_FILE_DIR:PointerLab>/resources/fonts/")
