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
               src/plain/tests_ubi_error_handling_leb.c
               src/plain/tests_ubi_error_handling_volume.c
               src/plain/tests_ubi_boundary.c
               src/plain/tests_ubi_recovery.c
               src/plain/tests_ubi_recovery_reserved.c
               src/plain/tests_ubi_fault_injection.c
               src/plain/tests_ubi_io_faults.c
               src/plain/tests_ubi_init_errors.c
               src/plain/tests_ubi_init_errors_geometry.c
               src/plain/tests_ubi_hil_smoke.c
               src/plain/tests_ubi_concurrency.c
               src/plain/tests_ubi_erased_val.c
               src/plain/tests_ubi_mutation_gate.c
               src/plain/tests_ubi_vol_id_watermark.c
               src/plain/tests_ubi_io_defensive.c)

if(CONFIG_FLASH_SIMULATOR)
    target_sources(app PRIVATE src/plain/tests_ubi_stress.c
                               src/plain/tests_ubi_torture.c
                               src/plain/tests_ubi_stress_longrun.c)
endif()

target_include_directories(app PRIVATE src/common)
target_include_directories(app PRIVATE src/secure)
target_include_directories(app PRIVATE ../lib/src/plain)

target_sources(app PRIVATE src/common/arrays.c)

if(CONFIG_UBI_SECURE)
    target_include_directories(app PRIVATE ../lib/src/secure)
    target_include_directories(app PRIVATE ../lib/src/plain)
    target_sources(app PRIVATE src/secure/tests_ubi_secure_api.c
                               src/secure/tests_ubi_secure_attach.c
                               src/secure/tests_ubi_secure_device.c
                               src/secure/tests_ubi_secure_volumes.c
                               src/secure/tests_ubi_secure_write_read.c
                               src/secure/tests_ubi_secure_map_unmap.c
                               src/secure/tests_ubi_secure_erase.c
                               src/secure/tests_ubi_secure_mixed.c
                               src/secure/tests_ubi_secure_tamper.c
                               src/secure/tests_ubi_secure_runtime_policy.c
                               src/secure/tests_ubi_secure_runtime_policy_budgets.c
                               src/secure/tests_ubi_secure_recovery.c
                               src/secure/tests_ubi_secure_forensic.c
                               src/secure/tests_ubi_secure_error_handling.c
                               src/secure/tests_ubi_secure_error_handling_volume.c
                               src/secure/tests_ubi_secure_error_handling_leb.c
                               src/secure/tests_ubi_secure_fault_injection.c
                               src/secure/tests_ubi_secure_mutation_gate.c
                               src/secure/tests_ubi_secure_vol_id_watermark.c
                               src/secure/tests_ubi_secure_crypto_faults.c
                               src/secure/tests_ubi_secure_coverage.c
                               src/secure/tests_ubi_secure_defensive.c
                               src/secure/tests_ubi_secure_defensive_io_hooks.c
                               src/secure/tests_ubi_secure_defensive_reserved.c
                               src/secure/tests_ubi_secure_replay.c
                               src/secure/tests_ubi_secure_anchor.c)
    if(CONFIG_FLASH_SIMULATOR)
        target_sources(app PRIVATE src/secure/tests_ubi_secure_concurrency.c
                                   src/secure/tests_ubi_secure_stress.c)
    endif()
    # Coexistence test requires a second flash partition labelled
    # `ubi_partition_2` in the active devicetree (provided by native_sim
    # overlays, absent on HW boards).
    dt_nodelabel(ubi_part2_path NODELABEL "ubi_partition_2")
    if(DEFINED ubi_part2_path)
        target_sources(app PRIVATE src/secure/tests_ubi_secure_coexistence.c)
    endif()
    if(CONFIG_UBI_SECURE_LEB_CHUNKED)
        target_sources(app PRIVATE src/secure/tests_ubi_secure_chunked.c)
    endif()
endif()
