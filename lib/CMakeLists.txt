cmake_minimum_required(VERSION 3.16)

project(ubi)
      
zephyr_library()
zephyr_library_sources(${CMAKE_CURRENT_SOURCE_DIR}/src/ubi.c ${CMAKE_CURRENT_SOURCE_DIR}/src/ubi_utils.c)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# target_compile_options(${ZEPHYR_CURRENT_LIBRARY} PRIVATE -Werror -Wextra -pedantic)
