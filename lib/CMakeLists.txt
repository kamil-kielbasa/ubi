cmake_minimum_required(VERSION 3.16)

project(ubi)

zephyr_library_sources(${CMAKE_CURRENT_SOURCE_DIR}/src/ubi.c)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
