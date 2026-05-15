cmake_minimum_required(VERSION 3.16)

project(ubi)
      
zephyr_library()
zephyr_library_sources(
    # facade
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi.c
    # common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common/ubi_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common/ubi_mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common/ubi_partition_guard.c
    # plain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_core_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_core_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_volume.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_leb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_io_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_io_data.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain/ubi_plain_flash_res_peb.c
)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/secure
)

if(CONFIG_UBI_SECURE)
    zephyr_library_sources(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_core_init.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_backend.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_budget.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_crypto.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_ser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_reserved.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_io.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_runtime.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_volume.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_leb.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_anchor.c
    )
    zephyr_library_link_libraries(mbedTLS)
endif()

if(CONFIG_UBI_SECURE_TEST_FAULT_INJECTION)
    zephyr_library_sources(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/secure/ubi_secure_test_hooks.c
    )
endif()

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
