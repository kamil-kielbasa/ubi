cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_sample)

if(CONFIG_UBI_CRYPTO)
    zephyr_library_sources(src/main_secure.c)
else()
    zephyr_library_sources(src/main.c)
endif()
