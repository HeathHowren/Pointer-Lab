# A 32-bit build of src/payload/SpeedHook.cpp.
#
# Separate CMake project for the same reason tests/helper32 is one: MSVC's
# architecture comes from the generator, not from a target property, so a Win32
# binary needs its own configure. ExternalProject_Add in the top-level
# CMakeLists drives it.
#
# The source is shared verbatim with the 64-bit payload, and that is the point.
# A DLL has to match the process it is loaded into, so both builds must exist;
# there is nothing bitness-specific in the code, so any divergence between them
# would be the compiler's doing rather than ours.

cmake_minimum_required(VERSION 3.28)
project(PointerLabSpeed32 CXX)

# Static CRT, matching the parent project. A payload that needed the VC++
# redistributable would fail to load in exactly the processes it is most wanted
# in, and the failure would look like the injection not working.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(PointerLabSpeed32 SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../payload/SpeedHook.cpp)
target_compile_features(PointerLabSpeed32 PRIVATE cxx_std_20)
target_compile_definitions(PointerLabSpeed32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(PointerLabSpeed32 PROPERTIES OUTPUT_NAME "PointerLabSpeed32")

if(MSVC)
    target_compile_options(PointerLabSpeed32 PRIVATE /W4 /permissive- /Zc:__cplusplus)
    if(POINTERLAB_WARNINGS_AS_ERRORS)
        target_compile_options(PointerLabSpeed32 PRIVATE /WX)
    endif()
endif()
