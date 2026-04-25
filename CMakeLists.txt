cmake_minimum_required(VERSION 3.15)
project(cortex_m_emu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /wd4100 /wd4200 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion
                        -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
    else()
        add_compile_options(-O0 -g)
    endif()
endif()

include_directories(include)

set(CORE_SRC
    src/core/cpu.c
    src/core/bus.c
    src/core/decoder.c
    src/core/executor.c
    src/core/nvic.c
    src/core/run.c
    src/core/fpu.c
    src/core/gdb.c
    src/periph/uart.c
    src/periph/systick.c
    src/periph/scb.c
    src/periph/mpu.c
    src/periph/stm32.c
    src/periph/dwt.c
)

add_library(cortex_m_core STATIC ${CORE_SRC})

add_executable(cortex-m tools/main.c)
target_link_libraries(cortex-m PRIVATE cortex_m_core)
if(WIN32)
    target_link_libraries(cortex-m PRIVATE ws2_32)
    target_link_libraries(cortex_m_core PRIVATE ws2_32)
endif()

enable_testing()
add_subdirectory(tests)
