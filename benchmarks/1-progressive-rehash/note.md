# 渐进式rehash笔记

## 测试方法

**确保 server 和 client 在不同 CPU 上执行**：服务端固定到 CPU 2，所有客户端固定到 CPU 3。两者必须是容器允许使用的不同 CPU；可用 `taskset -pc $$` 查看允许的 CPU 列表。比较两个版本时保持 CPU 编号、请求参数和编译选项一致。

脚本执行五轮，每轮启动空实例，先随机键增长、再同键覆盖，保存完整输出，最后只停止本轮启动的服务端。执行前需要释放 6379 端口。

```bash
(
set -euo pipefail
binary=./redlet-progressive-rehash
server_cpu=2
client_cpu=3

[[ -x "$binary" ]]
[[ "$server_cpu" != "$client_cpu" ]]
taskset -c "$server_cpu" true
taskset -c "$client_cpu" true
if taskset -c "$client_cpu" redis-cli -h 127.0.0.1 -p 6379 PING >/dev/null 2>&1; then
    echo '6379 已有服务，请先停止旧实例。' >&2
    exit 1
fi

results=$(mktemp -d "/tmp/redlet-$(basename "$binary").XXXXXX")
echo "结果目录：$results"
value=$(printf '%064d' 0 | tr '0' x)
server_pid=
cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for round in 1 2 3 4 5; do
    taskset -c "$server_cpu" "$binary" >"$results/$round-server.log" 2>&1 &
    server_pid=$!
    ready=false
    for attempt in {1..100}; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            cat "$results/$round-server.log" >&2
            exit 1
        fi
        if taskset -c "$client_cpu" redis-cli -h 127.0.0.1 -p 6379 PING 2>/dev/null | grep -qx PONG; then
            ready=true
            break
        fi
        sleep 0.05
    done
    if [[ "$ready" != true ]]; then
        echo '服务端启动超时。' >&2
        exit 1
    fi

    taskset -pc "$server_pid"
    taskset -c "$client_cpu" redis-benchmark -h 127.0.0.1 -p 6379 -c 1 -P 1 -n 1000000 -r 1000000000 --precision 3 SET bench:__rand_int__ "$value" \
        2>&1 | tee "$results/$round-growth.txt"
    taskset -c "$client_cpu" redis-benchmark -h 127.0.0.1 -p 6379 -c 1 -P 1 -n 100000 --precision 3 SET bench:control "$value" \
        2>&1 | tee "$results/$round-control.txt"
    cleanup
done
)
```


环境：现有 Ubuntu 24.04 / Linux aarch64 开发容器，GCC 13.3.0、redis-benchmark 7.0.15；编译参数为 `-std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror`。server 固定 CPU 2，client 固定 CPU 3，地址为 `127.0.0.1:6379`，`-c 1 -P 1`，value 为 64 字节。每轮均启动新进程、使用空键空间，增长测试后紧接同键覆盖。


## 查看指标

- 查看末尾 Summary 的 `max`、`p99`、`req/s`，重点观察 `max`。
- 各实现重复至少五轮，比较每轮 `max` 的中位数、最差轮次及吞吐。

## 修改前结果

基线提交：`0045647458d45664984027ddb0298bc48165ade5`，使用 `std::unordered_map`。


| 轮次 | 场景 | 请求数 | 吞吐 req/s | p99 ms | max ms |
| --- | --- | --- | --- | --- | --- |
| 1 | 随机键增长 | 1000000 | 34444.75 | 0.047 | 63.103 |
| 1 | 同键覆盖 | 100000 | 35137.04 | 0.047 | 3.263 |
| 2 | 随机键增长 | 1000000 | 34626.04 | 0.055 | 71.999 |
| 2 | 同键覆盖 | 100000 | 36127.17 | 0.039 | 0.311 |
| 3 | 随机键增长 | 1000000 | 35293.29 | 0.039 | 64.479 |
| 3 | 同键覆盖 | 100000 | 36390.10 | 0.039 | 0.087 |
| 4 | 随机键增长 | 1000000 | 35414.53 | 0.039 | 67.071 |
| 4 | 同键覆盖 | 100000 | 36179.45 | 0.039 | 0.175 |
| 5 | 随机键增长 | 1000000 | 34871.15 | 0.039 | 61.087 |
| 5 | 同键覆盖 | 100000 | 34965.04 | 0.039 | 0.175 |

五轮汇总：

| 场景 | 吞吐中位数 req/s | p99 中位数 ms | 各轮 max 中位数 ms | 最差轮次 max ms |
| --- | --- | --- | --- | --- |
| 随机键增长 | 34871.15 | 0.039 | 64.479 | 71.999 |
| 同键覆盖 | 36127.17 | 0.039 | 0.175 | 3.263 |

## 修改后结果

测试提交：`0dbb5a5960db59dab2fe8d6983ad88270d795504`，使用带渐进式rehash的哈希表。

| 轮次 | 场景 | 请求数 | 吞吐 req/s | p99 ms | max ms |
| --- | --- | --- | --- | --- | --- |
| 1 | 随机键增长 | 1000000 | 34765.68 | 0.039 | 5.943 |
| 1 | 同键覆盖 | 100000 | 35919.54 | 0.039 | 0.431 |
| 2 | 随机键增长 | 1000000 | 32702.18 | 0.055 | 19.375 |
| 2 | 同键覆盖 | 100000 | 29949.09 | 0.047 | 0.223 |
| 3 | 随机键增长 | 1000000 | 35118.52 | 0.039 | 15.583 |
| 3 | 同键覆盖 | 100000 | 36509.68 | 0.039 | 0.111 |
| 4 | 随机键增长 | 1000000 | 35124.70 | 0.039 | 6.255 |
| 4 | 同键覆盖 | 100000 | 35752.59 | 0.039 | 0.711 |
| 5 | 随机键增长 | 1000000 | 34939.38 | 0.039 | 2.695 |
| 5 | 同键覆盖 | 100000 | 36456.43 | 0.039 | 0.127 |

五轮汇总：

| 场景 | 吞吐中位数 req/s | p99 中位数 ms | 各轮 max 中位数 ms | 最差轮次 max ms |
| --- | --- | --- | --- | --- |
| 随机键增长 | 34939.38 | 0.039 | 6.255 | 19.375 |
| 同键覆盖 | 35919.54 | 0.039 | 0.223 | 0.711 |

本轮对比：

- 随机键增长：吞吐中位数变化 **+0.20%**；各轮 max 中位数从 **64.479 ms** 降至 **6.255 ms**，下降 **90.30%**；最差轮次 max 从 **71.999 ms** 降至 **19.375 ms**。
- 同键覆盖：吞吐中位数变化 **-0.57%**；各轮 max 中位数从 **0.175 ms** 变为 **0.223 ms**，最差轮次 max 从 **3.263 ms** 变为 **0.711 ms**。