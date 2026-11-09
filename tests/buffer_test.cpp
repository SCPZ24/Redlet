#include "../src/buffer/output_buffer.hpp"

#include <iostream>
#include <random>

// Linker wrapping injects syscall failures without production test switches.
static int fail_allocation_after = -1;
static std::size_t live_mappings = 0;
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t size) {
    if(fail_allocation_after == 0) throw std::bad_alloc();
    if(fail_allocation_after > 0) --fail_allocation_after;
    return __real__Znwm(size);
}
static bool fail_mmap = false;
static bool fail_advice = false;
static bool fail_unmap = false;
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_madvise(void*, size_t, int);
extern "C" int __real_munmap(void*, size_t);
extern "C" void* __wrap_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if(fail_mmap){ errno = ENOMEM; return MAP_FAILED; }
    void* result = __real_mmap(addr, length, prot, flags, fd, offset);
    if(result != MAP_FAILED) ++live_mappings;
    return result;
}
extern "C" int __wrap_madvise(void* addr, size_t length, int advice) {
    if(fail_advice){ errno = EINVAL; return -1; }
    return __real_madvise(addr, length, advice);
}
extern "C" int __wrap_munmap(void* addr, size_t length) {
    if(fail_unmap){ errno = EINVAL; return -1; }
    const int result = __real_munmap(addr, length);
    if(result == 0) --live_mappings;
    return result;
}

void check_counts(const ChunkPool& pool) {
    const auto stats = pool.stats();
    assert(stats.slab_count * PageSlab::chunk_count ==
           stats.in_use_chunks + stats.warm_free_chunks + stats.cold_free_chunks);
    assert(stats.mapped_bytes == stats.slab_count * PageSlab::chunk_count * pool.chunk_size());
    assert(stats.empty_slab_count <= stats.slab_count);
}

PoolConfig small_config() {
    PoolConfig config;
    config.max_mapped_bytes = static_cast<std::size_t>(sysconf(_SC_PAGESIZE)) * PageSlab::chunk_count * 2;
    config.warm_high_chunks = 4;
    config.warm_target_chunks = 2;
    config.warm_min_idle_ms = 10;
    config.empty_slab_idle_ms = 100;
    config.keep_empty_slabs = 0;
    config.max_advice_calls_per_tick = 2;
    config.maintenance_budget_us = 1000000;
    return config;
}

void check_pool() {
    ChunkPool pool;
    auto config = small_config();
    auto invalid = config;
    invalid.max_mapped_bytes = 1;
    bool rejected = false;
    try { pool.initialize(invalid); } catch(const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    pool.initialize(config);
    assert(pool.stats().slab_count == 0);
    const auto initial_mappings = live_mappings;
    // Fail Slab allocation, then fail ownership-vector growth after mmap succeeds.
    for(int allocation = 0; allocation < 2; ++allocation){
        fail_allocation_after = allocation;
        assert(pool.try_acquire() == nullptr);
        fail_allocation_after = -1;
        assert(pool.stats().slab_count == 0);
        assert(live_mappings == initial_mappings);
    }
    assert(pool.stats().metadata_allocation_failures == 2);
    fail_mmap = true;
    assert(pool.try_acquire() == nullptr);
    fail_mmap = false;
    assert(pool.stats().mmap_failures == 1);
    check_counts(pool);

    std::vector<ChunkControlBlock*> chunks;
    for(std::size_t i = 0; i < PageSlab::chunk_count * 2; ++i){
        auto* chunk = pool.try_acquire();
        assert(chunk && chunk->state == InUse && !chunk->prev && !chunk->next);
        assert(reinterpret_cast<std::uintptr_t>(chunk->page_ptr) % pool.chunk_size() == 0);
        std::memset(chunk->page_ptr, 0x5a, pool.chunk_size());
        chunks.push_back(chunk);
    }
    assert(pool.try_acquire() == nullptr);
    assert(pool.stats().capacity_failures == 1);
    assert(chunks[0]->slab != chunks[256]->slab);
    const auto now = monotonic_ms();
    // Keep a live page in each Slab while discarding all eligible free pages.
    for(std::size_t i = 0; i < chunks.size(); ++i){
        if(i != 0 && i != 256) pool.release(chunks[i], now);
    }
    pool.maintain(now + 9);
    assert(pool.stats().advice_calls == 0);
    fail_advice = true;
    pool.maintain(now + 10);
    fail_advice = false;
    assert(pool.stats().warm_free_chunks == 510);
    assert(pool.stats().advice_failures == 1);
    for(int i = 0; i < 254; ++i){
        pool.maintain(now + 10);
        check_counts(pool);
    }
    // Reclamation must continue below the high watermark down to the target.
    assert(pool.stats().warm_free_chunks == 2);
    assert(pool.stats().cold_free_chunks == 508);
    assert(pool.stats().slab_count == 2);
    assert(pool.stats().budget_exhaustions > 0);
    for(auto* chunk : {chunks[0], chunks[256]}){
        for(std::size_t i = 0; i < pool.chunk_size(); ++i) assert(chunk->page_ptr[i] == std::byte{0x5a});
    }
    auto* warm1 = pool.try_acquire();
    auto* warm2 = pool.try_acquire();
    assert(pool.stats().warm_hits == 2);
    auto* cold = pool.try_acquire();
    for(std::size_t i = 0; i < pool.chunk_size(); ++i) assert(cold->page_ptr[i] == std::byte{0});
    cold->page_ptr[0] = std::byte{0x33};
    for(auto* chunk : {warm1, warm2, cold, chunks[0], chunks[256]}) pool.release(chunk, now + 11);
    assert(pool.stats().empty_slab_count == 2);
    fail_unmap = true;
    pool.maintain(now + 111);
    fail_unmap = false;
    assert(pool.stats().slab_count == 2 && pool.stats().unmap_failures == 1);
    check_counts(pool);
    pool.maintain(now + 111);
    assert(pool.stats().slab_count == 1);
    // Acquire after swap-removal validates the moved Slab's ownership index.
    auto* reused = pool.try_acquire();
    pool.release(reused, now + 112);
    pool.maintain(now + 212);
    assert(pool.stats().slab_count == 0);
    check_counts(pool);
    for(int cycle = 0; cycle < 4; ++cycle){
        auto* chunk = pool.try_acquire();
        pool.release(chunk, now + 300 + cycle * 200);
        pool.maintain(now + 400 + cycle * 200);
        assert(pool.stats().mapped_bytes == 0);
    }

}

void check_empty_retention() {
    const auto before = live_mappings;
    {
        ChunkPool pool;
        auto config = small_config();
        config.keep_empty_slabs = 1;
        pool.initialize(config);
        std::vector<ChunkControlBlock*> chunks;
        for(std::size_t i = 0; i <= PageSlab::chunk_count; ++i){
            chunks.push_back(pool.try_acquire());
            assert(chunks.back());
        }
        const auto now = monotonic_ms();
        for(auto* chunk : chunks) pool.release(chunk, now);
        pool.maintain(now + 99);
        assert(pool.stats().slab_count == 2);
        pool.maintain(now + 100);
        assert(pool.stats().slab_count == 1);
        pool.maintain(now + 1000);
        assert(pool.stats().slab_count == 1);
        assert(pool.stats().empty_slab_count == 1);
        check_counts(pool);
    }
    assert(live_mappings == before);
}

void check_output() {
    const auto page = chunk_pool.chunk_size();
    OutputBuffer output;
    for(auto size : {std::size_t{0}, std::size_t{1}, page - 1, page, page + 1}){
        const std::string data(size, 'x');
        assert(output.append(data));
        assert(output.flatten_for_test() == data);
        output.consume(size, monotonic_ms());
        assert(output.empty() && chunk_pool.stats().in_use_chunks == 0);
    }
    assert(output.append(std::string(page - 3, 'a')));
    assert(output.append("hello"));
    assert(output.flatten_for_test() == std::string(page - 3, 'a') + "hello");
    output.clear();
    assert(output.append(std::string(page * 18, 'b')));
    output.consume(page - 1, monotonic_ms());
    iovec vectors[32];
    const auto batch = output.build_iovecs(vectors, 32, page * 16);
    assert(batch.count == 17 && batch.bytes == page * 16);
    assert(vectors[0].iov_len == 1);
    assert(output.build_iovecs(vectors, 1, page * 16).bytes == 1);
    assert(output.build_iovecs(nullptr, 0, 100).bytes == 0);
    assert(output.build_iovecs(vectors, 32, 0).bytes == 0);
    output.clear();

    std::mt19937 random(20261109);
    std::string expected;
    for(int step = 0; step < 30000; ++step){
        if(expected.size() < page * 20 && random() % 2 == 0){
            std::string data(random() % (page + 2), '\0');
            for(auto& byte : data) byte = static_cast<char>(random());
            assert(output.append(data));
            expected += data;
        }else{
            const auto consumed = random() % (expected.size() + 1);
            output.consume(consumed, monotonic_ms());
            expected.erase(0, consumed);
        }
        assert(output.flatten_for_test() == expected);
        assert(output.pending_bytes() == expected.size());
        const auto borrowed = chunk_pool.stats().in_use_chunks;
        assert(expected.empty() ? borrowed == 0 : borrowed * page - expected.size() < 2 * page);
        check_counts(chunk_pool);
    }
    output.clear();
    OutputBuffer limited(5);
    assert(limited.append("12345"));
    assert(!limited.append("x"));
    assert(!limited.can_append(std::numeric_limits<std::size_t>::max()));
    limited.clear();

    // Leave one available page, then require two. The temporary reservation must roll back.
    assert(output.append("old"));
    std::vector<ChunkControlBlock*> held;
    while(chunk_pool.stats().in_use_chunks < PageSlab::chunk_count * 2 - 1){
        held.push_back(chunk_pool.try_acquire());
        assert(held.back());
    }
    const auto before = chunk_pool.stats().in_use_chunks;
    assert(!output.append(std::string(page * 2, 'z')));
    assert(output.flatten_for_test() == "old");
    assert(chunk_pool.stats().in_use_chunks == before);
    for(auto* chunk : held) chunk_pool.release(chunk, monotonic_ms());
    output.clear();
    assert(chunk_pool.stats().in_use_chunks == 0);
}

int main() {
    chunk_pool.initialize(small_config());
    check_pool();
    check_empty_retention();
    check_output();
    std::cout << "ChunkPool and OutputBuffer checks passed\n";
}
