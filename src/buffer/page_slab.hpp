#pragma once

#include "chunk_control_block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <cerrno>
#include <system_error>
#include <sys/mman.h>

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

    // 调用方保证映射有效且无借用块。失败保留映射，成功清空映射句柄。
    bool try_unmap() noexcept {
        if(::munmap(base, bytes) != 0) return false;
        base = nullptr;
        return true;
    }

public:
    // page_size 由 ChunkPool 初始化时校验；这里 mmap 数据区并初始化 CCB。
    // 构造失败抛出异常，由 ChunkPool 的增长路径转换成分配失败。
    explicit PageSlab(std::size_t page_size) {
        bytes = page_size * chunk_count;
        void* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if(mapping == MAP_FAILED){
            throw std::system_error(errno, std::generic_category(), "mmap Slab");
        }
        base = static_cast<std::byte*>(mapping);
        for(std::size_t i = 0; i < controls.size(); ++i){
            controls[i].slab = this;
            controls[i].page_ptr = base + i * page_size;
            controls[i].state = FreeCold;
        }
    }

    ~PageSlab() noexcept {
        if(base) try_unmap();
    }

    PageSlab(const PageSlab&) = delete;
    PageSlab& operator=(const PageSlab&) = delete;
    PageSlab(PageSlab&&) = delete;
    PageSlab& operator=(PageSlab&&) = delete;
};
