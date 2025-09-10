#!/bin/bash

lib_src_dir="./lib/src"
lib_inc_dir="./lib/include"
tests_src_dir="./tests/src"

echo "Formating:"

# Format libedhoc.
echo "- (lib)    API & source code."
clang-format -i $lib_inc_dir/*.h
clang-format -i $lib_src_dir/*.c
clang-format -i $lib_src_dir/*.h

# Format tests.
echo "- (tests)  integration tests code."
clang-format -i $tests_src_dir/*.c