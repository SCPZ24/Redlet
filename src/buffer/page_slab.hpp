#pragma once

#include "chunk_control_block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class ChunkPool;

class PageSlab{
    friend class ChunkPool;

public:
    static constexpr std::size_t chunk_count = 256;

private:
    std::byte* base = nullptr;
    std::size_t bytes = 0;

    // CCB 内嵌在 Slab 中，数量固定，地址随 Slab 保持稳定。
    std::array<ChunkControlBlock, chunk_count> controls{};
    std::size_t borrowed_count = 0;

    // 在池的 Slab 所有权数组中的位置，支持交换末项后 O(1) 删除。
    std::size_t pool_index = 0;

    // 独立的空闲 Slab 链接；Chunk 此时仍在 Warm/Cold 队列中。
    PageSlab* empty_prev = nullptr;
    PageSlab* empty_next = nullptr;
    std::int64_t empty_since_ms = 0;
    bool in_empty_queue = false;

    // 仅在无借用块时调用。失败保留映射，成功清空映射句柄以免重复解除。
    bool try_unmap() noexcept;

public:
    // mmap 数据区并初始化每个 CCB，但不预触数据页。
    // 构造失败抛出异常，由 ChunkPool 的增长路径转换成分配失败。
    explicit PageSlab(std::size_t page_size);
    ~PageSlab() noexcept;

    PageSlab(const PageSlab&) = delete;
    PageSlab& operator=(const PageSlab&) = delete;
    PageSlab(PageSlab&&) = delete;
    PageSlab& operator=(PageSlab&&) = delete;
};
