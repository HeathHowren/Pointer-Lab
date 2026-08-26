# A 32-bit build of src/tutorial/main.cpp.
#
# Separate CMake project for the same reason src/payload32 is one: MSVC's
# architecture comes from the generator rather than from a target property, so a
# Win32 binary needs its own configure and its own binary directory.
#
# Both widths are shipped because pointer width is one of the things the
# tutorial teaches. Step 7's chain is four hops of four bytes here and four hops
# of eight bytes in the 64-bit build, and a reader who has done the same lesson
# both ways has learned something a paragraph cannot tell them.

cmake_minimum_required(VERSION 3.28)
project(PointerLabTutorial32 CXX)

# Static CRT, matching the parent project: the tutorial has to run on a machine
# with nothing installed on it, because that is the machine a reader is setting
# up when they reach it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PointerLabTutorial32 WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/../tutorial/main.cpp)
target_compile_features(PointerLabTutorial32 PRIVATE cxx_std_20)
target_compile_definitions(PointerLabTutorial32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(PointerLabTutorial32 PROPERTIES OUTPUT_NAME "PointerLabTutorial32")

if(MSVC)
    # /utf-8 because the lesson text is prose with real punctuation in it, and
    # without it MSVC reads the file in the system code page and the em dashes
    # reach the window as mojibake.
    target_compile_options(PointerLabTutorial32 PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    if(POINTERLAB_WARNINGS_AS_ERRORS)
        target_compile_options(PointerLabTutorial32 PRIVATE /WX)
    endif()
    # Identical COMDAT folding would merge two writer functions that happen to
    # compile to the same bytes, and then two steps would share one instruction
    # and one step's answer would solve the other.
    target_link_options(PointerLabTutorial32 PRIVATE /OPT:NOICF)
endif()
