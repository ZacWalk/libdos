cmake_minimum_required(VERSION 3.21)

# cc/ is C; everything else is C++.
project(ovrwin LANGUAGES C CXX VERSION 1.0.0)

if(NOT MSVC)
    message(FATAL_ERROR "ovrwin targets the Windows SDK through MSVC; no other toolchain is supported.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically linked CRT, so the executables run with no redistributable. Set
# before the platform layer is pulled in, so it is built the same way.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_FLAGS_DEBUG "/Od /Zi /RTC1 /D_DEBUG")
set(CMAKE_C_FLAGS_RELEASE "/O2 /Oi /Gy /Zi /DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /RTC1 /D_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Oi /Ot /Gy /Zi /DNDEBUG")

# Develop against a sibling platform-h checkout when one exists; CI and a fresh
# clone fall back to the pinned revision below.
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_PLATFORM_H
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../platform-h/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_PLATFORM_H "${CMAKE_CURRENT_SOURCE_DIR}/../platform-h"
        CACHE PATH "Local platform-h checkout to build against")
endif()

include(FetchContent)
FetchContent_Declare(platform_h
    GIT_REPOSITORY https://github.com/ZacWalk/platform-h.git
    GIT_TAG        a43d5c6b773b7629695a3cbaaaeafff0eea7135a
)
FetchContent_MakeAvailable(platform_h)

# The emulator. Platform independent: no Windows headers and no file system.
add_library(ovrwin_core STATIC
    src/bios.cpp
    src/chips.cpp
    src/cpu.cpp
    src/debug.cpp
    src/dos.cpp
    src/machine.cpp
    src/rom-font.cpp
    src/shell.cpp
    src/sound.cpp
    src/vga.cpp
)

target_include_directories(ovrwin_core PUBLIC src)

# The C compiler for 16-bit DOS. Platform independent too, and it opens no
# files: a caller supplies source text through cc_sources.
add_library(ovrwin_cc STATIC
    cc/emit.c
    cc/emit-x64.c
    cc/gen.cpp
    cc/lex.c
    cc/support.c
)

target_include_directories(ovrwin_cc PUBLIC cc)

# The Windows implementation of host_services: one directory presented to the
# emulated program as its whole drive. Shared by the app and the CLI.
add_library(ovrwin_host STATIC host/host-win32.cpp)
target_link_libraries(ovrwin_host PUBLIC ovrwin_core)

# Command line driver: the test suite, and the tools to inspect, disassemble,
# trace and headlessly run a program. Deliberately uses no platform layer.
add_executable(ovrwin_cli cli/main.cpp)
target_link_libraries(ovrwin_cli PRIVATE ovrwin_core ovrwin_cc ovrwin_host)

# The app: a window, the picture, the sound and the key mapping, all through
# the shared platform layer.
platform_add_app(ovrwin_app
    SOURCES     app/main.cpp
    DESCRIPTION "ovrwin DOS emulator"
    OUTPUT_NAME ovrwin
)

target_link_libraries(ovrwin_app PRIVATE ovrwin_core ovrwin_host)

set_target_properties(ovrwin_cli PROPERTIES OUTPUT_NAME ovrwincli)

foreach(target ovrwin_core ovrwin_cc ovrwin_host ovrwin_cli ovrwin_app)
    target_compile_options(${target} PRIVATE /W3 /MP)
endforeach()

foreach(target ovrwin_cli ovrwin_app)
    target_link_options(${target} PRIVATE
        /DEBUG
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )

    set_target_properties(${target} PROPERTIES
        DEBUG_POSTFIX -d
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/exe"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/exe"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/exe"
    )
endforeach()
