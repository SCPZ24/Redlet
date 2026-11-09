#pragma once

#include "page_slab.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../util.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unistd.h>

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

    void remove_free(ChunkControlBlock* chunk) noexcept {
        auto*& head = chunk->state == FreeWarm ? warm_head : cold_head;
        if(chunk->prev) chunk->prev->next = chunk->next;
        else head = chunk->next;
        if(chunk->next) chunk->next->prev = chunk->prev;
        if(chunk->state == FreeWarm){
            if(warm_tail == chunk) warm_tail = chunk->prev;
            --counters.warm_free_chunks;
        }else{
            --counters.cold_free_chunks;
        }
        chunk->prev = chunk->next = nullptr;
    }

    void push_free(ChunkControlBlock* chunk) noexcept {
        auto*& head = chunk->state == FreeWarm ? warm_head : cold_head;
        chunk->prev = nullptr;
        chunk->next = head;
        if(head) head->prev = chunk;
        head = chunk;
        if(chunk->state == FreeWarm){
            if(!warm_tail) warm_tail = chunk;
            ++counters.warm_free_chunks;
        }else{
            ++counters.cold_free_chunks;
        }
    }

    void remove_empty(PageSlab* slab) noexcept {
        if(slab->empty_prev) slab->empty_prev->empty_next = slab->empty_next;
        else empty_head = slab->empty_next;
        if(slab->empty_next) slab->empty_next->empty_prev = slab->empty_prev;
        else empty_tail = slab->empty_prev;
        slab->empty_prev = slab->empty_next = nullptr;
        --counters.empty_slab_count;
    }

    void push_empty(PageSlab* slab, std::int64_t now_ms) noexcept {
        slab->empty_since_ms = now_ms;
        slab->empty_prev = empty_tail;
        slab->empty_next = nullptr;
        if(empty_tail) empty_tail->empty_next = slab;
        else empty_head = slab;
        empty_tail = slab;
        ++counters.empty_slab_count;
    }

public:
    // 全局构造仅建立空状态，系统查询和配置校验在 initialize 中完成。
    ChunkPool() noexcept = default;
    ~ChunkPool() noexcept = default;

    // 调用方保证启动时初始化一次，成功后才进行借还和维护。
    // 系统查询或配置校验失败时抛出异常；初始化不预分配 Slab。
    void initialize(PoolConfig settings = {}) {
        const long raw_page_size = ::sysconf(_SC_PAGESIZE);
        if(raw_page_size <= 0) throw std::runtime_error("cannot read system page size");
        const auto size = static_cast<std::size_t>(raw_page_size);
        if(size > std::numeric_limits<std::size_t>::max() / PageSlab::chunk_count){
            throw std::invalid_argument("Slab size overflow");
        }
        const auto bytes = size * PageSlab::chunk_count;
        if(settings.max_mapped_bytes < bytes || settings.max_mapped_bytes % bytes != 0 ||
           settings.warm_target_chunks > settings.warm_high_chunks ||
           settings.warm_min_idle_ms < 0 || settings.empty_slab_idle_ms < 0 ||
           settings.maintenance_interval_ms <= 0 || settings.maintenance_budget_us <= 0 ||
           settings.max_advice_calls_per_tick == 0 || settings.max_unmap_slabs_per_tick == 0){
            throw std::invalid_argument("invalid ChunkPool configuration");
        }
        const auto now = monotonic_ms();
        if(settings.maintenance_interval_ms > std::numeric_limits<std::int64_t>::max() - now){
            throw std::invalid_argument("maintenance interval overflow");
        }
        config = settings;
        page_size = size;
        slab_bytes = bytes;
        maintenance_due_ms = now + config.maintenance_interval_ms;
    }

    std::size_t chunk_size() const noexcept { return page_size; }

    // Warm 优先，其次 Cold，最后按需增长。容量或分配失败返回 nullptr。
    // 成功返回的块已脱链、游标清零，并计入所属 Slab 的借用数。
    ChunkControlBlock* try_acquire() noexcept {
        if(!warm_head && !cold_head){
            if(slab_bytes > config.max_mapped_bytes - counters.mapped_bytes){
                ++counters.capacity_failures;
                return nullptr;
            }
            try{
                auto slab = std::make_unique<PageSlab>(page_size);
                slab->pool_index = slabs.size();
                slabs.push_back(std::move(slab));
            }catch(const std::bad_alloc&){
                ++counters.metadata_allocation_failures;
                return nullptr;
            }catch(const std::system_error&){
                ++counters.mmap_failures;
                return nullptr;
            }
            PageSlab* slab = slabs.back().get();
            ++counters.slab_count;
            ++counters.growth_count;
            counters.mapped_bytes += slab_bytes;
            for(auto& chunk : slab->controls) push_free(&chunk);
            push_empty(slab, monotonic_ms());
        }
        auto* chunk = warm_head ? warm_head : cold_head;
        if(chunk->state == FreeWarm) ++counters.warm_hits;
        else ++counters.cold_hits;
        remove_free(chunk);
        auto* slab = chunk->slab;
        if(slab->borrowed_count == 0) remove_empty(slab);
        ++slab->borrowed_count;
        ++counters.in_use_chunks;
        counters.peak_in_use_chunks = std::max(counters.peak_in_use_chunks, counters.in_use_chunks);
        chunk->sent = chunk->written = 0;
        chunk->state = InUse;
        return chunk;
    }

    // 调用方保证 CCB 有效、属于本池、处于 InUse，并且已经摘链。
    // 清游标并归还 Warm，只更新元数据；now_ms 使用单调时钟。
    void release(ChunkControlBlock* chunk, std::int64_t now_ms) noexcept {
        auto* slab = chunk->slab;
        --slab->borrowed_count;
        --counters.in_use_chunks;
        chunk->sent = chunk->written = 0;
        chunk->free_since_ms = now_ms;
        chunk->state = FreeWarm;
        push_free(chunk);
        if(slab->borrowed_count == 0) push_empty(slab, now_ms);
    }

    // 有界地回收 Warm 页及完全空闲 Slab，系统调用失败保留原状态。
    void maintain(std::int64_t now_ms) {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const auto elapsed_us = [&] {
            return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        };
        bool exhausted = false;
        std::size_t advice_count = 0;
        std::size_t unmap_count = 0;
        ++counters.maintenance_calls;
        if(counters.warm_free_chunks > config.warm_high_chunks) reclaim_active = true;
        while(reclaim_active && counters.warm_free_chunks > config.warm_target_chunks){
            auto* chunk = warm_tail;
            if(now_ms - chunk->free_since_ms < config.warm_min_idle_ms) break;
            if(advice_count == config.max_advice_calls_per_tick || elapsed_us() >= config.maintenance_budget_us){
                exhausted = true;
                break;
            }
            ++advice_count;
            ++counters.advice_calls;
            if(::madvise(chunk->page_ptr, page_size, MADV_DONTNEED) != 0){
                ++counters.advice_failures;
                break;
            }
            remove_free(chunk);
            chunk->state = FreeCold;
            push_free(chunk);
        }
        while(empty_head && counters.empty_slab_count > config.keep_empty_slabs){
            auto* slab = empty_head;
            if(now_ms - slab->empty_since_ms < config.empty_slab_idle_ms) break;
            if(unmap_count == config.max_unmap_slabs_per_tick || elapsed_us() >= config.maintenance_budget_us){
                exhausted = true;
                break;
            }
            ++unmap_count;
            ++counters.unmap_calls;
            if(!slab->try_unmap()){
                ++counters.unmap_failures;
                break;
            }
            // 映射解除后只操作独立的 CCB；提交阶段没有分配或回调。
            for(auto& chunk : slab->controls) remove_free(&chunk);
            remove_empty(slab);
            const auto index = slab->pool_index;
            if(index != slabs.size() - 1){
                std::swap(slabs[index], slabs.back());
                slabs[index]->pool_index = index;
            }
            slabs.pop_back();
            --counters.slab_count;
            counters.mapped_bytes -= slab_bytes;
        }
        if(counters.warm_free_chunks <= config.warm_target_chunks) reclaim_active = false;
        if(exhausted) ++counters.budget_exhaustions;
        const auto duration = static_cast<std::uint64_t>(elapsed_us());
        counters.maintenance_total_us += duration;
        counters.maintenance_max_us = std::max(counters.maintenance_max_us, duration);
        const auto max_time = std::numeric_limits<std::int64_t>::max();
        maintenance_due_ms = now_ms > max_time - config.maintenance_interval_ms
            ? max_time : now_ms + config.maintenance_interval_ms;
    }

    std::int64_t next_maintenance_ms() const noexcept { return maintenance_due_ms; }

    PoolStats stats() const noexcept { return counters; }

    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    ChunkPool(ChunkPool&&) = delete;
    ChunkPool& operator=(ChunkPool&&) = delete;
};

inline ChunkPool chunk_pool;
