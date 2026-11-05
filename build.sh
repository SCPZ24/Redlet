#!/usr/bin/env bash
set -euo pipefail

output=./redlet
while getopts ':o:' option; do
    case "$option" in
        o) output=$OPTARG ;;
        *) echo "Usage: $0 [-o output]" >&2; exit 1 ;;
    esac
done
shift "$((OPTIND - 1))"
if [[ $# -ne 0 || -z "$output" ]]; then
    echo "Usage: $0 [-o output]" >&2
    exit 1
fi

# Run inside the Redlet Linux development container.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror \
    src/main.cpp -o "$output"

echo "Built $output"
