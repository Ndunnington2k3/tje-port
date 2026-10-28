cmake_minimum_required(VERSION 3.20)
project(tje_port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(TJE_BUILD_SDL2 "Build the SDL2 desktop executable (tje)" ON)
option(TJE_BUILD_HEADLESS "Build the headless test executable (tje_headless)" ON)
option(TJE_FETCH_SDL2 "Download and build SDL2 if it is not installed" ON)

file(GLOB TJE_GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/generated/*.c)
if(NOT TJE_GENERATED_SOURCES)
    message(FATAL_ERROR
        "src/generated is empty. Run tools/generate.py first to translate the game code:\n"
        "  python tools/generate.py --disasm <path to disassembly 'master' folder> --rom <ROM file>")
endif()

# Translated game code + Genesis model, shared by every executable.
add_library(tje_core STATIC
    src/core/m68k_rt.c
    src/core/genesis.c
    src/core/vdp.c
    src/core/io.c
    src/core/sound.c
    third_party/nuked-opn2/ym3438.c
    third_party/z80/z80.c
    ${TJE_GENERATED_SOURCES})
target_include_directories(tje_core PUBLIC src src/generated)
target_include_directories(tje_core PRIVATE third_party/nuked-opn2 third_party/z80)

if(MSVC)
    # The generated functions are large and goto-heavy; keep MSVC happy.
    target_compile_options(tje_core PRIVATE /bigobj /wd4102 /wd4146 /wd4018)
    target_compile_definitions(tje_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(tje_core PRIVATE -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

if(TJE_BUILD_HEADLESS)
    add_executable(tje_headless src/main.c src/platform/null/platform_null.c)
    target_link_libraries(tje_headless PRIVATE tje_core)
endif()

if(TJE_BUILD_SDL2)
    find_package(SDL2 CONFIG QUIET)
    if(NOT SDL2_FOUND AND TJE_FETCH_SDL2)
        include(FetchContent)
        message(STATUS "SDL2 not found - downloading SDL2 2.30.9")
        FetchContent_Declare(SDL2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL2)
    endif()
    add_executable(tje WIN32 src/main.c src/platform/sdl2/platform_sdl2.c)
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(tje PRIVATE tje_core SDL2::SDL2-static)
    else()
        target_link_libraries(tje PRIVATE tje_core SDL2::SDL2)
    endif()
endif()
