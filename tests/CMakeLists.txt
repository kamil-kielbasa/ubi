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
               src/plain/tests_ubi_device.c
               src/plain/tests_ubi_volumes.c
               src/plain/tests_ubi_map_unmap.c
               src/plain/tests_ubi_write_read.c
               src/plain/tests_ubi_erase.c
               src/plain/tests_ubi_mixed.c
               src/plain/tests_ubi_error_handling.c
               src/plain/tests_ubi_boundary.c
               src/plain/tests_ubi_recovery.c
               src/plain/tests_ubi_fault_injection.c
               src/plain/tests_ubi_io_faults.c
               src/plain/tests_ubi_init_errors.c
               src/plain/tests_ubi_hil_smoke.c
               src/plain/tests_ubi_concurrency.c
               src/plain/tests_ubi_erased_val.c
               src/plain/tests_ubi_mutation_gate.c
               src/plain/tests_ubi_vol_id_watermark.c)

if(CONFIG_FLASH_SIMULATOR)
    target_sources(app PRIVATE src/plain/tests_ubi_stress.c
                               src/plain/tests_ubi_torture.c
                               src/plain/tests_ubi_stress_longrun.c)
endif()

target_include_directories(app PRIVATE src/common)
target_include_directories(app PRIVATE src/secure)

if(CONFIG_UBI_CRYPTO)
    target_sources(app PRIVATE src/secure/tests_ubi_secure_api.c
                               src/secure/tests_ubi_secure_attach.c)
endif()
