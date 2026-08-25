# 3.28 is required for EXCLUDE_FROM_ALL support in FetchContent_Declare, which
# keeps dependency-internal targets (Keystone's kstool and fuzz suite) out of
# the default build.
cmake_minimum_required(VERSION 3.28)

project(PointerLab VERSION 2.0.0 LANGUAGES C CXX)

# The C++ standard is deliberately NOT set globally. Keystone vendors a
# C++14-era LLVM fork that uses std::unary_function and std::iterator, both
# removed in C++17, so forcing C++20 project-wide fails to compile it.
# First-party targets opt into C++20 explicitly via target_compile_features.

# Single-config generators (Ninja, Makefiles) have no default build type; without
# this they silently produce an unoptimized, non-NDEBUG binary.
get_property(_pl_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _pl_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Static CRT so the shipped executable needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Keystone still declares cmake_minimum_required(2.8.7). CMake 3.31 warns and
# CMake 4.0 refuses outright; this keeps it configurable on both.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for old dependencies")

option(POINTERLAB_BUILD_TESTS "Build the Pointer Lab unit tests" ON)
option(POINTERLAB_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies (all pinned; see THIRD_PARTY_NOTICES.md)
# ---------------------------------------------------------------------------

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # Pinned commit on the docking branch. Tracking the branch itself made two
    # builds a week apart different programs.
    GIT_TAG b61e56346a92cfcaf1f43a545ca37b0b32239654
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    EXCLUDE_FROM_ALL
)

# Zydis: x86-64 decoder + formatter (MIT).
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS      OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN    OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN        OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG a2278f1d254e492f6a6b39f6cb5d1f5d515659dc # v4.1.1
    EXCLUDE_FROM_ALL
)

# Keystone: x86-64 assembler (GPLv2 - this is why Pointer Lab ships as GPLv2).
# X86 only; building every LLVM target would take many times longer.
set(BUILD_LIBS_ONLY             ON    CACHE BOOL   "" FORCE)
set(LLVM_TARGETS_TO_BUILD       "X86" CACHE STRING "" FORCE)
set(KEYSTONE_BUILD_STATIC_RUNTIME ON  CACHE BOOL   "" FORCE)
FetchContent_Declare(
    keystone
    GIT_REPOSITORY https://github.com/keystone-engine/keystone.git
    GIT_TAG dc7932ef2b2c4a793836caec6ecab485005139d6 # 0.9.2
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(imgui lua zydis keystone)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_compile_features(imgui PUBLIC cxx_std_20)
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

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h
    @ONLY
)

# ---------------------------------------------------------------------------
# Warning configuration, applied only to first-party targets
# ---------------------------------------------------------------------------

add_library(pointerlab_warnings INTERFACE)
if(MSVC)
    target_compile_options(pointerlab_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if(POINTERLAB_WARNINGS_AS_ERRORS)
        target_compile_options(pointerlab_warnings INTERFACE /WX)
    endif()
endif()

# ---------------------------------------------------------------------------
# pointerlab_core - everything except the entry point and the ImGui frontend,
# so the unit tests can link the real logic instead of a copy of it.
# ---------------------------------------------------------------------------

add_library(pointerlab_core STATIC
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
    src/infra/Settings.cpp
    src/platform_win32/Win32Platform.cpp
    src/scripting/LuaConsole.cpp
    src/scripting/LuaScanner.cpp
    src/services/RuntimeServices.cpp
    src/storage/ProjectStore.cpp
)

# UiApp is one class across several translation units, grouped by area. It was a
# single 2500-line file, roughly five times the next largest, and the only
# component with no test coverage.
set(POINTERLAB_UI_SOURCES
    src/ui/UiApp.cpp
    src/ui/UiLayout.cpp
    src/ui/UiMenu.cpp
    src/ui/UiPanelsTarget.cpp
    src/ui/UiPanelsScan.cpp
    src/ui/UiPanelsMemory.cpp
    src/ui/UiPanelsTools.cpp
    src/ui/UiProject.cpp
    src/ui/UiWindows.cpp
)

# PUBLIC so the executable and the test binary inherit C++20 as well.
target_compile_features(pointerlab_core PUBLIC cxx_std_20)

target_include_directories(pointerlab_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    # Keystone predates usage requirements and uses directory-scoped
    # include_directories(), so the target carries no include path of its own.
    ${keystone_SOURCE_DIR}/include
)

target_compile_definitions(pointerlab_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(pointerlab_core
    PUBLIC
        lua54
        Zydis
        keystone
    PRIVATE
        pointerlab_warnings
        d3d11
        dxgi
        dwmapi
        comctl32
        psapi
        dbghelp
        advapi32
)

# ---------------------------------------------------------------------------
# PointerLab executable
# ---------------------------------------------------------------------------

add_executable(PointerLab WIN32
    src/main.cpp
    src/app/Application.cpp
    ${POINTERLAB_UI_SOURCES}
    resources/PointerLab.rc
)

# The .rc includes the generated Version.h and references the icon/manifest by
# relative path, so it needs both directories on the resource include path.
target_include_directories(PointerLab PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)
set_source_files_properties(resources/PointerLab.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated;${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(PointerLab PRIVATE
    pointerlab_core
    pointerlab_warnings
    imgui
)

# The .rc embeds resources/app.manifest as resource 1. The linker also emits a
# default manifest under the same id, which collides (CVT1100); suppress it and
# let the explicit manifest win.
target_link_options(PointerLab PRIVATE "/MANIFEST:NO")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(POINTERLAB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / package - so the release archive is produced by the build rather
# than assembled by hand.
# ---------------------------------------------------------------------------

install(TARGETS PointerLab RUNTIME DESTINATION .)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
    DESTINATION .
)

# The Lua reference and the project-file spec are the two documents someone is
# most likely to want while using the tool rather than while reading the repo.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/lua-api.md
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/iretable-format.md
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/architecture.md
    DESTINATION docs
)

set(CPACK_PACKAGE_NAME "PointerLab")
set(CPACK_PACKAGE_VENDOR "Pointer Lab")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "PointerLab-v${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Windows x64 user-mode memory research tool")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
