#!/usr/bin/env bash
# Run inside the existing Redlet Linux development container, from /workspace.
set -euo pipefail
cd /workspace
scripts=benchmarks/3-chunk-pool/scripts
suffix=
if [[ ${1:-} == --nodelay ]]; then
    suffix=-nodelay
    shift
fi
if [[ $# != 0 ]]; then
    echo "Usage: $0 [--nodelay]" >&2
    exit 1
fi
export REDLET_BENCH_NODELAY=0
if [[ -n "$suffix" ]]; then export REDLET_BENCH_NODELAY=1; fi
results=benchmarks/3-chunk-pool/results/2026-11-09$suffix
mkdir -p "$results"
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread "$scripts/load.cpp" -o /tmp/redlet-load
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread "$scripts/pool_probe.cpp" -o /tmp/redlet-pool-probe
baseline_dir=$(mktemp -d /tmp/redlet-b0-diag.XXXXXX)
git archive e281a562ce5cae6e6f858b6772de2df7540cda69 | tar -x -C "$baseline_dir"
optimized_dir=/workspace
if [[ -n "$suffix" ]]; then
    patch -d "$baseline_dir" -p1 < "$scripts/nodelay.patch"
    bash "$baseline_dir/build.sh" -o /workspace/redlet-64kb-buffer-nodelay
    ./build.sh -o /workspace/redlet-chunk-pool-nodelay
else
    optimized_dir=$(mktemp -d /tmp/redlet-o-off.XXXXXX)
    git archive e259da431380485633f2ce5dd5d42e61bc42388b | tar -x -C "$optimized_dir"
fi
bash "$baseline_dir/build.sh" -o /tmp/redlet-b0-verified
bash "$optimized_dir/build.sh" -o /tmp/redlet-o-verified
cmp "redlet-64kb-buffer$suffix" /tmp/redlet-b0-verified
cmp "redlet-chunk-pool$suffix" /tmp/redlet-o-verified
sha256sum "redlet-64kb-buffer$suffix" /tmp/redlet-b0-verified "redlet-chunk-pool$suffix" /tmp/redlet-o-verified > "$results/binary-verification.txt"
for version in b0 o; do
    includes="$optimized_dir"
    pool=1
    if [[ "$version" == b0 ]]; then includes="$baseline_dir"; pool=0; fi
    g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread -DPOOL_VERSION="$pool" -I"$includes" \
        "$scripts/diagnostic.cpp" -Wl,--wrap=epoll_wait,--wrap=send,--wrap=sendmsg,--wrap=memmove -o "/tmp/redlet-$version-diag"
done
python3 "$scripts/compare.py" manifest
for suite in throughput memory mixed; do
    python3 "$scripts/compare.py" "$suite"
done
python3 "$scripts/compare.py" cycles --repeats 1
taskset -c 0 /tmp/redlet-pool-probe > "$results/pool-probe.csv" 2> "$results/pool-probe.log"
python3 "$scripts/diagnose.py"
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread -DCLIENT_QUICKACK "$scripts/load.cpp" -o /tmp/redlet-load-quickack
python3 "$scripts/ack_probe.py"
python3 "$scripts/copy_probe.py"
cp "$results/copy-diagnostic/summary.json" "$results/copy-summary.json"
./tests/run.sh > "$results/correctness.log"
python3 "$scripts/summarize.py" > "$results/summary.txt"
python3 "$scripts/plot.py"
python3 "$scripts/compare.py" manifest
