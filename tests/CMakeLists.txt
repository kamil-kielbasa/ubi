cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_tests)

target_compile_options(app PRIVATE
    -Werror
    -Wextra
    -Wshadow
    -Wdouble-promotion
    -Wformat=2
    -Wnull-dereference
    -Wunused
    -Wno-unused-parameter
)
target_compile_definitions(app PRIVATE _POSIX_C_SOURCE=200809L)

target_sources(app PRIVATE
               src/tests_ubi_device.c
               src/tests_ubi_volumes.c
               src/tests_ubi_map_unmap.c
               src/tests_ubi_write_read.c
               src/tests_ubi_erase.c
               src/tests_ubi_mixed.c
               src/tests_ubi_error_handling.c
               src/tests_ubi_boundary.c
               src/tests_ubi_recovery.c
               src/tests_ubi_fault_injection.c
               src/tests_ubi_hil_smoke.c
               src/tests_ubi_concurrency.c)

if(CONFIG_FLASH_SIMULATOR)
    target_sources(app PRIVATE src/tests_ubi_stress.c
                               src/tests_ubi_torture.c
                               src/tests_ubi_stress_longrun.c)
endif()
