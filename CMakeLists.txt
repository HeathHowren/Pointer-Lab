# 3.28 is required for EXCLUDE_FROM_ALL support in FetchContent_Declare, which
# keeps dependency-internal targets (Keystone's kstool and fuzz suite) out of
# the default build.
cmake_minimum_required(VERSION 3.28)

project(PointerLab VERSION 3.1.1 LANGUAGES C CXX)

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

# nlohmann/json: the JSON reader and writer behind the MCP server (MIT).
# Header-only, and the only dependency here that is not a whole subsystem.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # v3.11.3
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(imgui lua zydis keystone nlohmann_json)

# Marked SYSTEM so the first-party /W4 /WX does not apply to a 25 000-line
# header. Third-party warnings are exempt everywhere else in this file by being
# kept off the pointerlab_warnings target; a header-only library has no target
# of its own to keep them off, so the exemption has to be spelled out here.
get_target_property(_pl_json_includes nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nlohmann_json PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_pl_json_includes}")

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
    src/domain/Structure.cpp
    src/domain/TargetSession.cpp
    src/engine_aa/AutoAssembler.cpp
    src/engine_asm/Assembler.cpp
    src/engine_debug/AccessWatch.cpp
    src/engine_disasm/Disassembler.cpp
    src/engine_export/TrainerExport.cpp
    src/engine_inject/Injector.cpp
    src/engine_patch/PatchRegistry.cpp
    src/engine_pointer/PointerScanner.cpp
    src/engine_scan/MemoryScanner.cpp
    src/engine_speed/SpeedController.cpp
    src/engine_struct/Dissector.cpp
    src/engine_symbols/ExportResolver.cpp
    src/engine_symbols/SymbolTable.cpp
    src/infra/CrashHandler.cpp
    src/infra/Logger.cpp
    src/infra/Paths.cpp
    src/infra/Settings.cpp
    # The MCP tool surface is one registry split by engine group, for the same
    # reason UiApp is split by panel group: in one file it would be the largest
    # translation unit in the tree.
    src/mcp/McpHttp.cpp
    src/mcp/McpProtocol.cpp
    src/mcp/McpServer.cpp
    src/mcp/McpTools.cpp
    src/mcp/McpToolsAnalysis.cpp
    src/mcp/McpToolsControl.cpp
    src/platform_win32/Win32Platform.cpp
    src/scripting/LuaConsole.cpp
    src/scripting/LuaScanner.cpp
    src/services/RuntimeServices.cpp
    src/services/UiCommands.cpp
    src/storage/ProjectStore.cpp
)

# UiApp is one class across several translation units, grouped by area. It was a
# single 2500-line file, roughly five times the next largest, and the only
# component with no test coverage.
set(POINTERLAB_UI_SOURCES
    src/ui/UiApp.cpp
    src/ui/UiAutomation.cpp
    src/ui/UiLayout.cpp
    src/ui/UiMenu.cpp
    src/ui/UiPanelsTarget.cpp
    src/ui/UiPanelsScan.cpp
    src/ui/UiPanelsSymbols.cpp
    src/ui/UiPanelsScripts.cpp
    src/ui/UiPanelsStructures.cpp
    src/ui/UiPanelsSpeed.cpp
    src/ui/UiPanelsAccess.cpp
    src/ui/UiPanelsMcp.cpp
    src/ui/UiPanelsMemory.cpp
    src/ui/UiPanelsPatches.cpp
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
        nlohmann_json::nlohmann_json
    PRIVATE
        pointerlab_warnings
        d3d11
        dxgi
        dwmapi
        comctl32
        psapi
        dbghelp
        advapi32
        # The MCP server's loopback listener, and the system RNG behind its
        # session token. Nothing else in Pointer Lab opens a socket.
        ws2_32
        bcrypt
        # The PNG encoder behind screenshot(). A system component, not a new
        # dependency: a figure has to come out as a real image file, and adding
        # an image library to write one would be out of proportion.
        windowscodecs
        ole32
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
# Speed-hack payloads
#
# Two builds of one source, because a DLL has to match the architecture of the
# process it is loaded into and Pointer Lab attaches to both. They are put next
# to PointerLab.exe, which is where infra::Paths::speedPayload looks.
# ---------------------------------------------------------------------------

add_library(PointerLabSpeed64 SHARED src/payload/SpeedHook.cpp)
target_compile_features(PointerLabSpeed64 PRIVATE cxx_std_20)
target_compile_definitions(PointerLabSpeed64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PointerLabSpeed64 PRIVATE pointerlab_warnings)
set_target_properties(PointerLabSpeed64 PROPERTIES OUTPUT_NAME "PointerLabSpeed64")
add_dependencies(PointerLab PointerLabSpeed64)

option(POINTERLAB_BUILD_PAYLOAD32 "Build the 32-bit speed-hack payload" ON)

if(POINTERLAB_BUILD_PAYLOAD32 AND MSVC)
    include(ExternalProject)
    ExternalProject_Add(pointerlab_speed32
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/payload32
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/payload32-build
        CMAKE_GENERATOR ${CMAKE_GENERATOR}
        CMAKE_GENERATOR_PLATFORM Win32
        CMAKE_ARGS
            -DPOINTERLAB_WARNINGS_AS_ERRORS=${POINTERLAB_WARNINGS_AS_ERRORS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
        # Without this the sub-build runs exactly once, ever: ExternalProject
        # stamps the build step on first success and does not know that
        # SpeedHook.cpp changed. A 32-bit payload silently months behind the
        # 64-bit one is the worst possible version of that bug, because the two
        # are supposed to be the same source and the difference only shows up in
        # the targets nobody tests.
        BUILD_ALWAYS 1
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/$<CONFIG>/PointerLabSpeed32.dll
    )
    ExternalProject_Add_Step(pointerlab_speed32 stage
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                <BINARY_DIR>/$<CONFIG>/PointerLabSpeed32.dll
                $<TARGET_FILE_DIR:PointerLab>/PointerLabSpeed32.dll
        DEPENDEES build
        ALWAYS TRUE
    )
    add_dependencies(PointerLab pointerlab_speed32)
endif()

# ---------------------------------------------------------------------------
# The tutorial target
#
# A first-class shipped artifact rather than a sample. Everything else Pointer
# Lab can be pointed at belongs to somebody else, which means a reader's first
# lesson depends on a third-party download that may have changed since the book
# was written. This one is built from this repository and checks its own answers.
# ---------------------------------------------------------------------------

add_executable(PointerLabTutorial WIN32 src/tutorial/main.cpp)
target_compile_features(PointerLabTutorial PRIVATE cxx_std_20)
target_compile_definitions(PointerLabTutorial PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PointerLabTutorial PRIVATE pointerlab_warnings)
# See src/tutorial32/CMakeLists.txt: folding two writer functions into one would
# make one step's answer solve another.
#
# /utf-8 because the lesson text is prose with real punctuation in it, and
# without it MSVC reads the file in the system code page and the em dashes reach
# the window as mojibake.
if(MSVC)
    target_link_options(PointerLabTutorial PRIVATE /OPT:NOICF)
    target_compile_options(PointerLabTutorial PRIVATE /utf-8)
endif()

option(POINTERLAB_BUILD_TUTORIAL32 "Build the 32-bit tutorial target" ON)

if(POINTERLAB_BUILD_TUTORIAL32 AND MSVC)
    include(ExternalProject)
    ExternalProject_Add(pointerlab_tutorial32
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/tutorial32
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/tutorial32-build
        CMAKE_GENERATOR ${CMAKE_GENERATOR}
        CMAKE_GENERATOR_PLATFORM Win32
        CMAKE_ARGS
            -DPOINTERLAB_WARNINGS_AS_ERRORS=${POINTERLAB_WARNINGS_AS_ERRORS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
        # See the payload above: without this the sub-build happens once and
        # every later edit to the shared source reaches only the 64-bit build.
        BUILD_ALWAYS 1
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/$<CONFIG>/PointerLabTutorial32.exe
    )
    ExternalProject_Add_Step(pointerlab_tutorial32 stage
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                <BINARY_DIR>/$<CONFIG>/PointerLabTutorial32.exe
                $<TARGET_FILE_DIR:PointerLab>/PointerLabTutorial32.exe
        DEPENDEES build
        ALWAYS TRUE
    )
    add_dependencies(PointerLab pointerlab_tutorial32)
endif()

add_dependencies(PointerLab PointerLabTutorial)

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

# Beside the executable, because that is where SpeedController looks for them.
# A zip missing these has a speed hack that reports a broken install rather than
# one that quietly does nothing, but it is still a broken install.
install(TARGETS PointerLabSpeed64 RUNTIME DESTINATION .)
if(TARGET pointerlab_speed32)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/payload32-build/$<CONFIG>/PointerLabSpeed32.dll
            DESTINATION .)
endif()

# Both widths of the tutorial, as first-class artifacts. A reader who has to go
# and find a practice target before the first lesson usually finds a different
# version of one.
install(TARGETS PointerLabTutorial RUNTIME DESTINATION .)
if(TARGET pointerlab_tutorial32)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tutorial32-build/$<CONFIG>/PointerLabTutorial32.exe
            DESTINATION .)
endif()

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
    DESTINATION .
)

# The Lua reference and the project-file spec are the two documents someone is
# most likely to want while using the tool rather than while reading the repo.
# The MCP reference ships for a stronger reason: it is where the server's "no
# confirmation once started" posture is written down, and that has to travel
# with the binary rather than live only in the repository.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/lua-api.md
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/mcp-api.md
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/iretable-format.md
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/architecture.md
    DESTINATION docs
)

# Shipped rather than left in the repository, because it is the answer to "how
# were these figures made" and that question is asked by someone holding the zip.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/capture-figures.lua
    DESTINATION scripts
)

set(CPACK_PACKAGE_NAME "PointerLab")
set(CPACK_PACKAGE_VENDOR "Pointer Lab")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "PointerLab-v${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Windows x64 user-mode memory research tool")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
