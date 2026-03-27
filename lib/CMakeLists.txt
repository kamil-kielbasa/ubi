cmake_minimum_required(VERSION 3.16)

project(ubi)
      
zephyr_library()
zephyr_library_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_volume.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_leb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_res_peb.c
)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(${ZEPHYR_CURRENT_LIBRARY} PRIVATE
    -Werror
    -Wextra
    -Wshadow
    -Wdouble-promotion
    -Wformat=2
    -Wnull-dereference
    -Wunused
    -Wno-unused-parameter
)
target_compile_definitions(${ZEPHYR_CURRENT_LIBRARY} PRIVATE _POSIX_C_SOURCE=200809L)
