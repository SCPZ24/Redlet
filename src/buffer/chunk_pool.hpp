#pragma once

#include "page_slab.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct PoolConfig {
    std::size_t max_mapped_bytes = 256 * 1024 * 1024;

    std::size_t warm_high_chunks = 128;
    std::size_t warm_target_chunks = 64;
    std::int64_t warm_min_idle_ms = 2000;

    std::int64_t maintenance_interval_ms = 100;
    // 第一版逐页 advice，每次调用最多处理一页，失败后结束本轮页回收。
    std::size_t max_advice_calls_per_tick = 32;
    std::int64_t maintenance_budget_us = 200;

    std::int64_t empty_slab_idle_ms = 10000;
    std::size_t keep_empty_slabs = 1;
    std::size_t max_unmap_slabs_per_tick = 1;
};

struct PoolStats {
    std::size_t slab_count = 0;
    std::size_t empty_slab_count = 0;
    std::size_t mapped_bytes = 0;
    std::size_t in_use_chunks = 0;
    std::size_t warm_free_chunks = 0;
    std::size_t cold_free_chunks = 0;
    std::size_t peak_in_use_chunks = 0;

    std::uint64_t warm_hits = 0;
    std::uint64_t cold_hits = 0;
    std::uint64_t growth_count = 0;
    std::uint64_t capacity_failures = 0;
    std::uint64_t mmap_failures = 0;
    std::uint64_t metadata_allocation_failures = 0;

    std::uint64_t advice_calls = 0;
    std::uint64_t advice_failures = 0;
    std::uint64_t unmap_calls = 0;
    std::uint64_t unmap_failures = 0;
    std::uint64_t maintenance_calls = 0;
    std::uint64_t budget_exhaustions = 0;
    std::uint64_t maintenance_total_us = 0;
    std::uint64_t maintenance_max_us = 0;
};

// 所有借还和维护操作均由网络事件循环线程执行。
class ChunkPool {
private:
    PoolConfig config;
    PoolStats counters;
    bool initialized = false;
    std::size_t page_size = 0;
    std::size_t slab_bytes = 0;
    std::int64_t maintenance_due_ms = 0;
    bool reclaim_active = false;

    // 容器移动 unique_ptr 时，Slab 及其控制块地址保持稳定。
    std::vector<std::unique_ptr<PageSlab>> slabs;

    // Warm 从头部借出、归还，从尾部检查最老的回收候选。
    ChunkControlBlock* warm_head = nullptr;
    ChunkControlBlock* warm_tail = nullptr;
    // Cold 从头部插入和借出；CCB 保留双向链接供 Slab 释放时摘除。
    ChunkControlBlock* cold_head = nullptr;

    // 按进入完全空闲状态的时间排序，头部最老。
    PageSlab* empty_head = nullptr;
    PageSlab* empty_tail = nullptr;

public:
    // 全局构造仅建立空状态，系统查询和配置校验在 initialize 中完成。
    ChunkPool() noexcept = default;
    ~ChunkPool() noexcept;

    // 启动时调用一次。失败抛出异常且保留未初始化状态，不预分配 Slab。
    // 重复初始化作为调用错误处理；借还及维护要求初始化已成功。
    void initialize(PoolConfig settings = {});

    std::size_t chunk_size() const noexcept;

    // Warm 优先，其次 Cold，最后按需增长。容量或分配失败返回 nullptr。
    // 成功返回的块已脱链、游标清零，并计入所属 Slab 的借用数。
    ChunkControlBlock* try_acquire() noexcept;

    // 调用方先摘链。块须属于本池且处于 InUse，重复归还属于调用错误。
    // 清游标并归还 Warm，只更新元数据；now_ms 使用单调时钟。
    void release(ChunkControlBlock* chunk, std::int64_t now_ms) noexcept;

    // 有界地回收 Warm 页及完全空闲 Slab，系统调用失败保留原状态。
    void maintain(std::int64_t now_ms);
    std::int64_t next_maintenance_ms() const noexcept;
    PoolStats stats() const noexcept;

    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    ChunkPool(ChunkPool&&) = delete;
    ChunkPool& operator=(ChunkPool&&) = delete;
};

inline ChunkPool chunk_pool;
