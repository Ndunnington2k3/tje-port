cmake_minimum_required(VERSION 3.20)
project(tje_port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(TJE_PLATFORM_SDL2 "Build the SDL2 platform backend" ON)

# Sources are added as each layer lands (core, platform, generated, game).
