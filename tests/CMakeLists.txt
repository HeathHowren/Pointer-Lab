include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)

# A tiny, scriptable target process for the integration tests. Scanning a real
# second process is the only way to exercise the Win32 read/write path; scanning
# the test process itself would also mean scanning Catch2's own allocations.
add_executable(pointerlab_test_helper helper/main.cpp)
target_compile_features(pointerlab_test_helper PRIVATE cxx_std_20)
target_link_libraries(pointerlab_test_helper PRIVATE pointerlab_warnings)

# The same helper, built for x86, so the WOW64 path is covered by tests rather
# than by hope. Pointer Lab is a 64-bit process that must attach to 32-bit
# targets -- Assault Cube and most of the practice binaries in the wild are
# 32-bit -- and every layer that reasons about pointer width or instruction
# encoding takes a different branch for them.
#
# ExternalProject because MSVC's architecture comes from the generator, not from
# a target property: -A Win32 needs its own configure and its own binary dir.
# Optional, and off by default in a plain Debug configure, because it doubles
# the configure cost of a build most contributors run dozens of times a day.
option(POINTERLAB_BUILD_HELPER32 "Build the 32-bit test helper for the WOW64 tests" ON)

if(POINTERLAB_BUILD_HELPER32 AND MSVC)
    include(ExternalProject)
    ExternalProject_Add(pointerlab_helper32
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/helper32
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/helper32-build
        CMAKE_GENERATOR ${CMAKE_GENERATOR}
        CMAKE_GENERATOR_PLATFORM Win32
        CMAKE_ARGS
            -DPOINTERLAB_WARNINGS_AS_ERRORS=${POINTERLAB_WARNINGS_AS_ERRORS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
        # ExternalProject stamps its build step on first success and has no idea
        # that helper/main.cpp changed, so without this the x86 helper is frozen
        # at whatever it was the first time it built while the x64 one moves on.
        # The WOW64 tests would then be passing against a different program.
        BUILD_ALWAYS 1
        INSTALL_COMMAND ""
        # Next to the test executable, which is where HelperProcess looks.
        BUILD_BYPRODUCTS <BINARY_DIR>/$<CONFIG>/pointerlab_test_helper32.exe
    )
    ExternalProject_Add_Step(pointerlab_helper32 stage
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                <BINARY_DIR>/$<CONFIG>/pointerlab_test_helper32.exe
                $<TARGET_FILE_DIR:pointerlab_tests>/pointerlab_test_helper32.exe
        DEPENDEES build
        ALWAYS TRUE
    )
endif()

# The crash path is the one thing that cannot be tested in-process: a passing
# run ends with the process gone. This probe crashes on demand so the test can
# inspect what the handler left behind.
add_executable(pointerlab_crash_probe crash_probe/main.cpp)
target_compile_features(pointerlab_crash_probe PRIVATE cxx_std_20)
target_link_libraries(pointerlab_crash_probe PRIVATE pointerlab_core pointerlab_warnings)

add_executable(pointerlab_tests
    test_crash_handler.cpp
    test_domain.cpp
    test_project_store.cpp
    test_scanner.cpp
    test_scan_integration.cpp
    test_settings.cpp
    test_asm_disasm.cpp
    test_breakpoints.cpp
    test_hotkeys.cpp
    test_pointer.cpp
    test_lua.cpp
    test_workflow.cpp
    test_wow64.cpp
    test_access_watch.cpp
    test_patches.cpp
    test_scan_modes.cpp
    test_symbols.cpp
    test_autoassembler.cpp
    test_dissector.cpp
    test_trainer_export.cpp
    test_speed.cpp
)

# HelperProcess.h lives here rather than in src/, so the tests need this
# directory on their own include path.
target_include_directories(pointerlab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pointerlab_tests PRIVATE
    pointerlab_core
    pointerlab_warnings
    Catch2::Catch2WithMain
)

# The tests locate the helper next to their own executable, so it only has to
# exist by the time they run.
add_dependencies(pointerlab_tests pointerlab_test_helper pointerlab_crash_probe)

# SpeedController looks for the payload beside the *running* executable, which
# for the tests is this directory rather than the one PointerLab.exe lives in.
# Staged rather than looked up by a test-only code path, so the tests exercise
# the same path search the shipped tool does.
add_dependencies(pointerlab_tests PointerLabSpeed64)
add_custom_command(TARGET pointerlab_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:PointerLabSpeed64>
            $<TARGET_FILE_DIR:pointerlab_tests>/PointerLabSpeed64.dll
)

# The WOW64 tests skip rather than fail when this was not built, so the
# dependency is added only when it exists.
if(TARGET pointerlab_helper32)
    add_dependencies(pointerlab_tests pointerlab_helper32)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(pointerlab_tests)
