#pragma once

#include <cstddef>
#include <cstdint>

class PageSlab;


enum ChunkState{
    InUse,
    FreeWarm,
    FreeCold
};


struct ChunkControlBlock{
    PageSlab* slab;
    std::byte* page_ptr = nullptr;

    std::size_t sent = 0;
    std::size_t written = 0;

    ChunkState state;
    std::int64_t free_since_ms = 0;

    ChunkControlBlock* prev;
    ChunkControlBlock* next;
};
