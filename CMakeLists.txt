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

set(ENDFIELDS_LIB_SOURCES
    src/endfields.c
    src/ef_port.c
    src/ef_crc.c
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

add_executable(endfields_test src/main.c)
target_link_libraries(endfields_test PRIVATE endfields)

add_executable(endfields_embedded_test src/main_embedded.c)
target_link_libraries(endfields_embedded_test PRIVATE endfields_embedded)

enable_testing()
add_test(NAME endfields_rw COMMAND endfields_test)
add_test(NAME endfields_embedded COMMAND endfields_embedded_test)

install(TARGETS endfields endfields_embedded
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES src/endfields.h src/ef_config.h DESTINATION include/endfields)
