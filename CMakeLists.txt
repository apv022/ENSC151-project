cmake_minimum_required(VERSION 3.16)

project(FocusBird VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prefer an already-installed raylib package; otherwise fetch the pinned version
# used by this project so a fresh checkout can build without manual library setup.
find_package(raylib 5.0 QUIET)

if(NOT raylib_FOUND)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "Do not build raylib examples" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "Do not build raylib games" FORCE)
    # Clone with Git when it is available; otherwise download a source tarball
    # so a machine without Git (e.g. a ZIP download on Windows) still builds.
    find_package(Git QUIET)
    if(GIT_FOUND)
        FetchContent_Declare(
            raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 5.5
            GIT_SHALLOW TRUE
        )
    else()
        FetchContent_Declare(
            raylib
            URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
        )
    endif()
    FetchContent_MakeAvailable(raylib)
endif()

add_executable(focusbird
    src/main.cpp
    src/serial_port.cpp
)

target_include_directories(focusbird PRIVATE include)
target_link_libraries(focusbird PRIVATE raylib)

if(WIN32)
    target_compile_definitions(focusbird PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # Make the Visual Studio "Run" button start FocusBird instead of ALL_BUILD.
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT focusbird)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(focusbird PRIVATE m pthread dl rt X11)
endif()

if(MSVC)
    target_compile_options(focusbird PRIVATE /W4 /permissive-)
else()
    target_compile_options(focusbird PRIVATE -Wall -Wextra -Wpedantic)
endif()
