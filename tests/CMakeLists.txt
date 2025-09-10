cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_tests)

target_sources(app PRIVATE
               src/tests_ubi_device.c
               src/tests_ubi_volumes.c
               src/tests_ubi_map_unmap.c
               src/tests_ubi_write_read.c
               src/tests_ubi_erase.c
               src/tests_ubi_mixed.c)
