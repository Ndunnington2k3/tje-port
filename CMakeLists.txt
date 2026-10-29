cmake_minimum_required(VERSION 3.20)
project(tje_port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(TJE_BUILD_SDL2 "Build the SDL2 desktop executable (tje)" ON)
option(TJE_BUILD_HEADLESS "Build the headless test executable (tje_headless)" ON)
option(TJE_FETCH_SDL2 "Download and build SDL2 if it is not installed" ON)
option(TJE_BUILD_ISA_TEST "Build isa_test: every translated instruction vs a reference 68000 (downloads Musashi)" OFF)

file(GLOB TJE_GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/generated/gen_*.c)
if(NOT TJE_GENERATED_SOURCES)
    message(FATAL_ERROR
        "src/generated is empty. Run tools/generate.py first to translate the game code:\n"
        "  python tools/generate.py --disasm <path to disassembly 'master' folder> --rom <ROM file>")
endif()

# Hand-written replacements for translated routines (src/game). The translator must know
# which routines they replace (TJE_OVERRIDE markers); generate.py records that in
# src/generated/overrides.txt - make sure it is up to date.
file(GLOB TJE_GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/game/*.c)
set(_tje_markers "")
foreach(_f ${TJE_GAME_SOURCES})
    file(STRINGS ${_f} _lines REGEX "^[ \t]*TJE_OVERRIDE\\(0x[0-9A-Fa-f]+\\)")
    foreach(_l ${_lines})
        string(REGEX REPLACE ".*0x([0-9A-Fa-f]+).*" "\\1" _a "${_l}")
        string(TOUPPER "${_a}" _a)
        string(LENGTH "${_a}" _n)
        while(_n LESS 6)
            set(_a "0${_a}")
            math(EXPR _n "${_n} + 1")
        endwhile()
        list(APPEND _tje_markers ${_a})
    endforeach()
endforeach()
list(SORT _tje_markers)
set(_tje_generated "")
if(EXISTS ${CMAKE_SOURCE_DIR}/src/generated/overrides.txt)
    file(STRINGS ${CMAKE_SOURCE_DIR}/src/generated/overrides.txt _tje_generated)
    list(SORT _tje_generated)
endif()
if(NOT "${_tje_markers}" STREQUAL "${_tje_generated}")
    message(FATAL_ERROR
        "src/game replaces routines [${_tje_markers}] but src/generated was made for [${_tje_generated}].\n"
        "Re-run tools/generate.py after adding or removing TJE_OVERRIDE markers.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/generated/overrides.txt)

# Translated game code + Genesis model, shared by every executable.
add_library(tje_core STATIC
    src/core/m68k_rt.c
    src/core/genesis.c
    src/core/vdp.c
    src/core/io.c
    src/core/sound.c
    third_party/nuked-opn2/ym3438.c
    third_party/z80/z80.c
    ${TJE_GAME_SOURCES}
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

if(TJE_BUILD_ISA_TEST)
    # Musashi (MIT) as the reference 68000. It generates its opcode table with a small tool.
    include(FetchContent)
    FetchContent_Declare(musashi
        GIT_REPOSITORY https://github.com/kstenerud/Musashi.git
        GIT_TAG 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd)
    FetchContent_MakeAvailable(musashi)
    set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi_gen)
    add_executable(m68kmake ${musashi_SOURCE_DIR}/m68kmake.c)
    add_custom_command(
        OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GEN}
        COMMAND m68kmake ${MUSASHI_GEN} ${musashi_SOURCE_DIR}/m68k_in.c
        DEPENDS m68kmake ${musashi_SOURCE_DIR}/m68k_in.c)
    add_executable(isa_test
        tools/isa_test/isa_test.c
        ${CMAKE_SOURCE_DIR}/src/generated/isa_cases.c
        ${musashi_SOURCE_DIR}/m68kcpu.c
        ${musashi_SOURCE_DIR}/m68kdasm.c
        ${musashi_SOURCE_DIR}/softfloat/softfloat.c
        ${MUSASHI_GEN}/m68kops.c)
    target_include_directories(isa_test PRIVATE src tools/isa_test ${musashi_SOURCE_DIR} ${MUSASHI_GEN})
    if(NOT MSVC)
        target_compile_options(isa_test PRIVATE -w)
        target_link_libraries(isa_test PRIVATE m)
    endif()
endif()
