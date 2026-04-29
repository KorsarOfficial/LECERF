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
    src/core/jit.c
    src/core/codegen.c
    src/core/tt.c
    src/core/board.c
    src/periph/uart.c
    src/periph/systick.c
    src/periph/scb.c
    src/periph/mpu.c
    src/periph/stm32.c
    src/periph/dwt.c
    src/periph/eth.c
)

add_library(cortex_m_core STATIC ${CORE_SRC})

if(NOT LECERF_BUILD_PYTHON)
    add_executable(cortex-m tools/main.c)
    target_link_libraries(cortex-m PRIVATE cortex_m_core)
    if(WIN32)
        target_link_libraries(cortex-m PRIVATE ws2_32)
    endif()
endif()

if(WIN32)
    target_link_libraries(cortex_m_core PRIVATE ws2_32)
endif()

# liblecerf: thin C ABI shared library wrapping cortex_m_core
add_library(liblecerf SHARED src/lecerf_api.c)
set_target_properties(liblecerf PROPERTIES OUTPUT_NAME "liblecerf" PREFIX "")
target_link_libraries(liblecerf PRIVATE cortex_m_core)
if(WIN32)
    target_link_libraries(liblecerf PRIVATE ws2_32)
endif()

# lecerf-smoke: two-board CPU and TT-event isolation demo
if(NOT LECERF_BUILD_PYTHON)
    add_executable(lecerf-smoke tools/smoke.c)
    target_link_libraries(lecerf-smoke PRIVATE cortex_m_core)
    if(WIN32)
        target_link_libraries(lecerf-smoke PRIVATE ws2_32)
    endif()
endif()

# Install liblecerf into lecerf/ package directory when building wheel
install(TARGETS liblecerf
    LIBRARY DESTINATION lecerf
    RUNTIME DESTINATION lecerf
)

if(NOT LECERF_BUILD_PYTHON)
    enable_testing()
    add_subdirectory(tests)
endif()
