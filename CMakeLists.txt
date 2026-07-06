cmake_minimum_required(VERSION 3.16)

project(endfields C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ENDFIELDS_EMBEDDED_ONLY "Build RAM-only backend (no file I/O)" OFF)
option(ENDFIELDS_ENABLE_PREFETCH "Enable CPU prefetch hints on chase path" ON)
option(ENDFIELDS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ENDFIELDS_SANITIZE "Enable AddressSanitizer+UBSan (GCC/Clang)" OFF)
option(ENDFIELDS_TSAN "Enable ThreadSanitizer (GCC/Clang)" OFF)
option(ENDFIELDS_COVERAGE "Enable gcov/lcov code coverage (GCC/Clang)" OFF)
option(ENDFIELDS_CI_FAST "Shorter performance bench loops for CI" OFF)

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "Recommended compiler is GCC or Clang; current: ${CMAKE_C_COMPILER_ID}")
endif()

# MinGW gcc can fail silently when include paths are passed via @response files (CRLF).
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES 0)
endif()

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /utf-8)
    if(ENDFIELDS_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -pedantic)
    if(ENDFIELDS_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
    if(NOT WIN32)
        add_compile_definitions(_POSIX_C_SOURCE=200809L)
    endif()
endif()

if(ENDFIELDS_SANITIZE AND ENDFIELDS_TSAN)
    message(FATAL_ERROR "ENDFIELDS_SANITIZE and ENDFIELDS_TSAN are mutually exclusive")
endif()

if(ENDFIELDS_COVERAGE AND (ENDFIELDS_SANITIZE OR ENDFIELDS_TSAN))
    message(FATAL_ERROR "ENDFIELDS_COVERAGE is incompatible with ENDFIELDS_SANITIZE and ENDFIELDS_TSAN")
endif()

if(ENDFIELDS_COVERAGE)
    if(NOT (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
        message(FATAL_ERROR "ENDFIELDS_COVERAGE requires GCC or Clang")
    endif()
    add_compile_options(-O0 --coverage -fprofile-update=atomic)
    add_link_options(--coverage)
endif()

if(ENDFIELDS_SANITIZE)
    if(NOT (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
        message(FATAL_ERROR "ENDFIELDS_SANITIZE requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ENDFIELDS_TSAN)
    if(NOT (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
        message(FATAL_ERROR "ENDFIELDS_TSAN requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

set(ENDFIELDS_LIB_SOURCES
    src/endfields.c
    src/ef_port.c
    src/ef_crc.c
    src/ef_index.c
    src/ef_sb_layout.c
)

function(endfields_apply_target_options target_name)
    if(ENDFIELDS_ENABLE_PREFETCH)
        target_compile_definitions(${target_name} PUBLIC EF_ENABLE_PREFETCH)
    endif()
    if(NOT MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:-O2>
            $<$<CONFIG:RelWithDebInfo>:-O2>
            $<$<CONFIG:MinSizeRel>:-Os>
        )
    endif()
endfunction()

add_library(endfields STATIC ${ENDFIELDS_LIB_SOURCES})
target_include_directories(endfields PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
endfields_apply_target_options(endfields)

if(ENDFIELDS_EMBEDDED_ONLY)
    target_compile_definitions(endfields PUBLIC EF_PLATFORM_EMBEDDED)
    message(STATUS "endfields: embedded-only build (no file I/O)")
else()
    message(STATUS "endfields: platform=${CMAKE_SYSTEM_NAME}, backend=file+memory")
endif()

add_library(endfields_embedded STATIC ${ENDFIELDS_LIB_SOURCES})
target_include_directories(endfields_embedded PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(endfields_embedded PUBLIC EF_PLATFORM_EMBEDDED)
endfields_apply_target_options(endfields_embedded)

if(NOT ENDFIELDS_EMBEDDED_ONLY)
    add_executable(endfields_test src/main.c)
    target_link_libraries(endfields_test PRIVATE endfields)
    if(ENDFIELDS_CI_FAST)
        target_compile_definitions(endfields_test PRIVATE ENDFIELDS_CI_FAST=1)
    endif()
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(endfields_test PRIVATE Threads::Threads)
    endif()
endif()

add_executable(endfields_embedded_test src/main_embedded.c)
target_link_libraries(endfields_embedded_test PRIVATE endfields_embedded)

enable_testing()
if(NOT ENDFIELDS_EMBEDDED_ONLY)
    add_test(NAME endfields_rw COMMAND endfields_test)
    set_tests_properties(endfields_rw PROPERTIES TIMEOUT 900 LABELS "full")
endif()
add_test(NAME endfields_embedded COMMAND endfields_embedded_test)
set_tests_properties(endfields_embedded PROPERTIES TIMEOUT 180 LABELS "embedded")

install(TARGETS endfields endfields_embedded
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES src/endfields.h src/ef_config.h src/ef_index.h DESTINATION include/endfields)
