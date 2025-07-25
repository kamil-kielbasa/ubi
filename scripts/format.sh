#!/bin/bash

lib_src_dir="./lib/src"
lib_inc_dir="./lib/include"
sample_src_dir="./sample/src"
tests_src_dir="./tests/src"

echo "Formating:"

# Format libedhoc.
echo "- (lib)    API & source code."
clang-format -i $lib_inc_dir/*.h
clang-format -i $lib_src_dir/*.c

# Format sample.
echo "- (sample) sample code."
clang-format -i $sample_src_dir/*.c

# Format tests.
echo "- (tests)  integration tests code."
clang-format -i $tests_src_dir/*.c