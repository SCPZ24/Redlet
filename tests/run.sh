#!/usr/bin/env bash
# Run inside the Redlet Linux development container.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
build_dir=$(mktemp -d /tmp/redlet-tests.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
flags=(-std=c++17 -O1 -g -Dtest -Wall -Wextra -Werror -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie)
fi
for source in tests/*_test.cpp; do
    name=$(basename "$source" .cpp)
    link_flags=()
    if [[ "$name" == buffer_test ]]; then
        link_flags+=(-Wl,--wrap=mmap,--wrap=madvise,--wrap=munmap,--wrap=_Znwm)
    fi
    if [[ "$name" == connection_fault_test ]]; then
        link_flags+=(-Wl,--wrap=sendmsg)
    fi
    if [[ "$name" == server_loop_test ]]; then
        link_flags+=(-Wl,--wrap=epoll_wait)
    fi
    g++ "${flags[@]}" "$source" "${link_flags[@]}" -o "$build_dir/$name"
    "$build_dir/$name"
done
