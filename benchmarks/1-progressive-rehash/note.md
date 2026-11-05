# 渐进式rehash笔记

## 测试方法

启动Redlet进程。通过redis-benchmark直接测试。

```bash
value=$(printf '%064d' 0 | tr '0' x)
redis-benchmark -h 127.0.0.1 -c 1 -P 1 -n 1000000 -r 1000000000 --precision 3 SET bench:__rand_int__ "$value"
redis-benchmark -h 127.0.0.1 -c 1 -P 1 -n 100000 --precision 3 SET bench:control "$value"
```

## 查看指标

- 查看末尾 Summary 的 `max`、`p99` 和 `requests per second`。扩容稀少，重点观察 `max`，整体 p99 容易漏掉尖峰。
- 实现前后使用相同负载，各重复至少五轮，比较每轮 `max` 的中位数、最差轮次及吞吐，判断削峰效果和吞吐代价。

## 修改前结果

> 修改前最后一次提交 671a40f34313473a4f326605932538eec7526839

| 场景 | 请求数 | 吞吐 req/s | p99 ms | max ms |
| --- | --- | --- | --- | --- |
| 随机键增长 | 1000000 | 160333.50 | 0.015 | 74.943 |
| 同键覆盖 | 100000 | 182149.36 | 0.015 | 0.895 |


## 修改后结果

> 