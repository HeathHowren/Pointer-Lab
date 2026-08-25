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
    test_asm_disasm.cpp
    test_breakpoints.cpp
    test_hotkeys.cpp
    test_pointer.cpp
    test_lua.cpp
    test_workflow.cpp
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

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(pointerlab_tests)
