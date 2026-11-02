#!/usr/bin/env bash
set -euo pipefail
# Run from a clean 057a worktree; paths below match the measured environment.
source_dir=/workspace/zyx-unified-fixtures
artifact_dir=/workspace/zyx-link-tmp
clang=/workspace/zyx-sysroot/usr/bin/clang++-19
common=(-DBOOST_ATOMIC_DYN_LINK -DBOOST_ATOMIC_NO_LIB -DBOOST_FILESYSTEM_DYN_LINK -DBOOST_FILESYSTEM_NO_LIB -DZYX_HAS_LZ4=1 -DZYX_HAS_ZSTD=1 -I"$source_dir/include" -I"$source_dir/src" -I"$source_dir" -isystem /workspace/zyx-sysroot/usr/include -O3 -DNDEBUG -std=c++20 -fPIE)
for name in concurrent-current pin-release-space; do
 "$clang" "${common[@]}" -c "$artifact_dir/$name.cpp" -o "$artifact_dir/$name.o"
 LIBRARY_PATH=/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu "$clang" -O3 -DNDEBUG "$artifact_dir/$name.o" -o "$artifact_dir/$name" -Wl,-rpath,/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu /workspace/zyx-release-57fa/libzyx_core.a /workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libz.so /workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/liblz4.so.1.10.0 -lxxhash /workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libzstd.a /workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libboost_filesystem.so.1.83.0 /workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libboost_atomic.so.1.83.0 /workspace/antlr4-install/lib/libantlr4-runtime.a /workspace/zyx-release-57fa/libzyx_inputxx.a
done
