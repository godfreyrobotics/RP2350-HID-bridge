#!/usr/bin/env sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_dir="$repo_dir/build-host-tests"

mkdir -p "$output_dir"
gcc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -Wno-unused-function \
    -Wno-unused-variable \
    -Wno-missing-field-initializers \
    "$repo_dir/tests/test_firmware.c" \
    -lm \
    -o "$output_dir/test_firmware"

"$output_dir/test_firmware"
