#!/usr/bin/env bash
set -euo pipefail

# Run inside the Redlet Linux development container.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror \
    src/main.cpp -o redlet

echo "Built redlet"
