#include "../../../src/buffer/chunk_pool.hpp"

#include <fstream>
#include <iostream>
#include <thread>
#include <cstring>

long rss_kib() {
    std::ifstream input("/proc/self/smaps_rollup");
    std::string line;
    while(std::getline(input, line)) {
        if(line.rfind("Rss:", 0) == 0) return std::stol(line.substr(4));
    }
    throw std::runtime_error("RSS unavailable");
}

void snapshot(const char* phase, std::int64_t started, const ChunkPool& pool) {
    const auto s = pool.stats();
    std::cout << phase << ',' << monotonic_ms() - started << ',' << rss_kib() << ','
              << s.slab_count << ',' << s.mapped_bytes << ',' << s.in_use_chunks << ','
              << s.warm_free_chunks << ',' << s.cold_free_chunks << ',' << s.advice_calls << ','
              << s.unmap_calls << ',' << s.maintenance_calls << ',' << s.maintenance_total_us << ','
              << s.maintenance_max_us << ',' << s.budget_exhaustions << '\n';
}

int main() {
    ChunkPool pool;
    pool.initialize();
    const auto started = monotonic_ms();
    std::cout << "phase,t_ms,rss_kib,slabs,mapped,inuse,warm,cold,advice,unmap,maintenance,total_us,max_us,budget_exhaustions\n";
    snapshot("before", started, pool);
    std::vector<ChunkControlBlock*> chunks;
    for(std::size_t i = 0; i < PageSlab::chunk_count * 32; ++i) {
        auto* c = pool.try_acquire();
        if(!c) throw std::runtime_error("acquire failed");
        std::memset(c->page_ptr, 0x5a, pool.chunk_size());
        chunks.push_back(c);
    }
    snapshot("touched", started, pool);
    const auto released = monotonic_ms();
    for(std::size_t i = 0; i < chunks.size(); ++i) {
        if(i % PageSlab::chunk_count != 0) pool.release(chunks[i], released);
    }
    snapshot("one_live_per_slab", started, pool);
    while(pool.stats().warm_free_chunks > 64) {
        pool.maintain(monotonic_ms());
        snapshot("discarding", started, pool);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(monotonic_ms() - released > 60000) throw std::runtime_error("discard timeout");
    }
    std::size_t resident = 0;
    std::vector<unsigned char> residency(PageSlab::chunk_count);
    for(std::size_t i = 0; i < chunks.size(); i += PageSlab::chunk_count) {
        // Fresh Slabs are borrowed from the Cold head, so the last page borrowed has the base address.
        auto* base = chunks[i + PageSlab::chunk_count - 1]->page_ptr;
        if(mincore(base, pool.chunk_size() * PageSlab::chunk_count, residency.data()) != 0) {
            throw std::runtime_error("mincore failed");
        }
        for(auto byte : residency) resident += byte & 1;
        for(std::size_t offset = 0; offset < pool.chunk_size(); ++offset) {
            if(chunks[i]->page_ptr[offset] != std::byte{0x5a}) throw std::runtime_error("live page corrupted");
        }
    }
    std::cerr << "resident_pages_after_discard=" << resident << " ccb_bytes=" << sizeof(ChunkControlBlock)
              << " slab_bytes=" << sizeof(PageSlab) << '\n';
    snapshot("discarded", started, pool);
    for(std::size_t i = 0; i < chunks.size(); i += PageSlab::chunk_count) {
        pool.release(chunks[i], monotonic_ms());
    }
    while(pool.stats().slab_count > 1) {
        pool.maintain(monotonic_ms());
        snapshot("unmapping", started, pool);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(monotonic_ms() - released > 90000) throw std::runtime_error("unmap timeout");
    }
    snapshot("finished", started, pool);
}
