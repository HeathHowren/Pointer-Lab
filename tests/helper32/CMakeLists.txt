# A 32-bit build of tests/helper/main.cpp.
#
# This is a separate CMake project rather than another target in the main one
# because MSVC's architecture is fixed by the generator (-A x64), not per-target:
# a Win32 binary needs its own configure. ExternalProject_Add in ../CMakeLists.txt
# drives it.
#
# The source is shared verbatim with the 64-bit helper. That is the point --
# the WOW64 tests assert that the same program, built for x86, is scanned,
# chain-walked, disassembled and breakpointed correctly, so any divergence in
# the helper itself would weaken the comparison.

cmake_minimum_required(VERSION 3.28)
project(pointerlab_test_helper32 CXX)

# Matches the parent project, so the helper has no runtime DLL dependency the
# test rig would have to stage next to it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(pointerlab_test_helper32 ${CMAKE_CURRENT_SOURCE_DIR}/../helper/main.cpp)
target_compile_features(pointerlab_test_helper32 PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(pointerlab_test_helper32 PRIVATE /W4 /permissive- /Zc:__cplusplus)
    if(POINTERLAB_WARNINGS_AS_ERRORS)
        target_compile_options(pointerlab_test_helper32 PRIVATE /WX)
    endif()
endif()
