cmake_minimum_required(VERSION 3.21)

# cc/ is C; everything else is C++.
project(libdos LANGUAGES C CXX VERSION 1.0.0)

if(NOT MSVC)
    message(FATAL_ERROR "libdos targets the Windows SDK through MSVC; no other toolchain is supported.")
endif()

# A project that embeds libdos gets the libraries; the executables are built
# when libdos is the top-level project. Only dui needs the platform layer, so
# nothing else fetches it.
option(LIBDOS_BUILD_TOOLS "Build demu, the command line runner and test suite" ${PROJECT_IS_TOP_LEVEL})
option(LIBDOS_BUILD_APP "Build dui, the windowed app (fetches platform-h)" ${PROJECT_IS_TOP_LEVEL})

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

if(LIBDOS_BUILD_APP)
    # Develop against a sibling platform-h checkout when one exists; CI and a
    # fresh clone fall back to the pinned revision below.
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_PLATFORM_H
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../platform-h/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_PLATFORM_H "${CMAKE_CURRENT_SOURCE_DIR}/../platform-h"
            CACHE PATH "Local platform-h checkout to build against")
    endif()

    include(FetchContent)
    FetchContent_Declare(platform_h
        GIT_REPOSITORY https://github.com/ZacWalk/platform-h.git
        GIT_TAG        9383d73592d16bbb275913c15b96d5f6e907e1f4
    )
    FetchContent_MakeAvailable(platform_h)
endif()

# The emulator. Platform independent: no Windows headers and no file system.
add_library(libdos STATIC
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

target_include_directories(libdos PUBLIC src)

# The C compiler for 16-bit DOS. Platform independent too, and it opens no
# files: a caller supplies source text through cc_sources.
add_library(libdos_cc STATIC
    cc/emit.c
    cc/emit-x64.c
    cc/gen.cpp
    cc/lex.c
    cc/support.c
)

target_include_directories(libdos_cc PUBLIC cc)

# The Windows implementation of host_services: one directory presented to the
# emulated program as its whole drive. Shared by demu and dui.
add_library(libdos_host STATIC host/host-win32.cpp)
target_link_libraries(libdos_host PUBLIC libdos)

set(libdos_targets libdos libdos_cc libdos_host)
set(libdos_executables)

if(LIBDOS_BUILD_TOOLS)
    # demu: the command line runner, debugger and test suite. Runs a DOS
    # program headlessly, inspects, disassembles and traces one. Deliberately
    # uses no platform layer.
    add_executable(demu cli/main.cpp)
    target_link_libraries(demu PRIVATE libdos libdos_cc libdos_host)
    list(APPEND libdos_targets demu)
    list(APPEND libdos_executables demu)
endif()

if(LIBDOS_BUILD_APP)
    # dui: a window, the picture, the sound and the key mapping, all through
    # the shared platform layer.
    platform_add_app(dui
        SOURCES     app/main.cpp
        DESCRIPTION "dui - DOS emulator"
        OUTPUT_NAME dui
    )

    target_link_libraries(dui PRIVATE libdos libdos_host)
    list(APPEND libdos_targets dui)
    list(APPEND libdos_executables dui)
endif()

foreach(target ${libdos_targets})
    target_compile_options(${target} PRIVATE /W3 /MP)
endforeach()

foreach(target ${libdos_executables})
    target_link_options(${target} PRIVATE
        /DEBUG
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )

    set_target_properties(${target} PROPERTIES
        DEBUG_POSTFIX -d
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/exe"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/exe"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/exe"
    )
endforeach()

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    # The gates: demu's behavioural suite, then dui's offscreen self-check. A
    # new check of src/ or cc/ goes in the demu suite; there is no separate
    # unit test binary.
    if(LIBDOS_BUILD_TOOLS)
        add_test(NAME demu_suite COMMAND demu --test)
        set_tests_properties(demu_suite PROPERTIES
            TIMEOUT 120 LABELS libdos WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    endif()
    if(LIBDOS_BUILD_APP)
        add_test(NAME dui_selftest COMMAND dui -test)
        set_tests_properties(dui_selftest PROPERTIES
            TIMEOUT 120 LABELS libdos WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    endif()
endif()