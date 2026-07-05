cmake_minimum_required(VERSION 3.16)

project(endfields C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ENDFIELDS_EMBEDDED_ONLY "Build RAM-only backend (no file I/O)" OFF)
option(ENDFIELDS_ENABLE_PREFETCH "Enable CPU prefetch hints on chase path" ON)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(WARNING "Recommended compiler is GCC; current: ${CMAKE_C_COMPILER_ID}")
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic)
    if(NOT WIN32)
        add_compile_definitions(_POSIX_C_SOURCE=200809L)
    endif()
endif()

set(ENDFIELDS_SOURCES
    src/endfields.c
    src/ef_port.c
    src/ef_crc.c
    src/main.c
)

add_executable(endfields_test ${ENDFIELDS_SOURCES})
target_include_directories(endfields_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(ENDFIELDS_EMBEDDED_ONLY)
    target_compile_definitions(endfields_test PRIVATE EF_PLATFORM_EMBEDDED)
    message(STATUS "endfields: embedded-only build (no file I/O)")
else()
    message(STATUS "endfields: platform=${CMAKE_SYSTEM_NAME}, backend=file+memory")
endif()

if(ENDFIELDS_ENABLE_PREFETCH)
    target_compile_definitions(endfields_test PRIVATE EF_ENABLE_PREFETCH)
endif()

if(NOT MSVC)
    target_compile_options(endfields_test PRIVATE
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:MinSizeRel>:-Os>
    )
endif()

enable_testing()
add_test(NAME endfields_rw COMMAND endfields_test)

# Standalone embedded backend smoke test (always RAM-backed).
add_executable(endfields_embedded_test src/endfields.c src/ef_port.c src/ef_crc.c src/main_embedded.c)
target_include_directories(endfields_embedded_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(endfields_embedded_test PRIVATE EF_PLATFORM_EMBEDDED EF_ENABLE_PREFETCH)
if(NOT MSVC)
    target_compile_options(endfields_embedded_test PRIVATE -Wall -Wextra -pedantic -O2)
endif()
add_test(NAME endfields_embedded COMMAND endfields_embedded_test)
