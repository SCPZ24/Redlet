#pragma once

#include "chunk_pool.hpp"
#include "../config.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/uio.h>

struct IovBatch {
    std::size_t count = 0;
    std::size_t bytes = 0;
};

class OutputBuffer {
private:
    ChunkControlBlock* head = nullptr;
    ChunkControlBlock* tail = nullptr;
    std::size_t pending = 0;
    std::size_t limit;

public:
    explicit OutputBuffer(std::size_t max_pending_bytes = MAX_OUTPUT_BYTES) noexcept
        : limit(max_pending_bytes) {}
    ~OutputBuffer() { clear(); }

    std::size_t pending_bytes() const noexcept { return pending; }
    bool empty() const noexcept { return pending == 0; }
    bool can_append(std::size_t bytes) const noexcept { return bytes <= limit - pending; }

    bool append(std::string_view bytes) noexcept {
        if(!can_append(bytes.size())) return false;
        if(bytes.empty()) return true;
        const auto page_size = chunk_pool.chunk_size();
        if(page_size == 0) std::abort();
        const auto available = tail ? page_size - tail->written : 0;
        const auto extra = bytes.size() > available ? bytes.size() - available : 0;
        const auto needed = extra / page_size + (extra % page_size != 0);
        ChunkControlBlock* reserved = nullptr;
        for(std::size_t i = 0; i < needed; ++i){
            auto* chunk = chunk_pool.try_acquire();
            if(!chunk){
                const auto now = monotonic_ms();
                while(reserved){
                    auto* next = reserved->next;
                    reserved->prev = reserved->next = nullptr;
                    chunk_pool.release(reserved, now);
                    reserved = next;
                }
                return false;
            }
            chunk->next = reserved;
            if(reserved) reserved->prev = chunk;
            reserved = chunk;
        }
        // 预留完成后提交，只涉及指针、计数和 memcpy，不再执行分配。
        const auto appended = bytes.size();
        while(!bytes.empty()){
            if(!tail || tail->written == page_size){
                auto* chunk = reserved;
                reserved = reserved->next;
                if(reserved) reserved->prev = nullptr;
                chunk->next = nullptr;
                chunk->prev = tail;
                if(tail) tail->next = chunk;
                else head = chunk;
                tail = chunk;
            }
            const auto count = std::min(bytes.size(), page_size - tail->written);
            std::memcpy(tail->page_ptr + tail->written, bytes.data(), count);
            tail->written += count;
            bytes.remove_prefix(count);
        }
        assert(reserved == nullptr);
        pending += appended;
        return true;
    }

    bool push_back(char byte) noexcept { return append(std::string_view(&byte, 1)); }

    IovBatch build_iovecs(iovec* dst, std::size_t max_iov, std::size_t max_bytes) const noexcept {
        IovBatch batch;
        for(auto* chunk = head; chunk && batch.count < max_iov && batch.bytes < max_bytes;
            chunk = chunk->next){
            const auto count = std::min(chunk->written - chunk->sent, max_bytes - batch.bytes);
            dst[batch.count++] = {chunk->page_ptr + chunk->sent, count};
            batch.bytes += count;
        }
        return batch;
    }

    void consume(std::size_t bytes, std::int64_t now_ms) noexcept {
        if(bytes > pending) std::abort();
        pending -= bytes;
        while(bytes != 0){
            auto* chunk = head;
            const auto count = std::min(bytes, chunk->written - chunk->sent);
            chunk->sent += count;
            bytes -= count;
            if(chunk->sent == chunk->written){
                head = chunk->next;
                if(head) head->prev = nullptr;
                else tail = nullptr;
                chunk->prev = chunk->next = nullptr;
                chunk_pool.release(chunk, now_ms);
            }
        }
    }

    void clear(std::int64_t now_ms) noexcept { consume(pending, now_ms); }
    void clear() noexcept { if(head) clear(monotonic_ms()); }

#ifdef test
    std::string flatten_for_test() const {
        std::string result;
        result.reserve(pending);
        for(auto* chunk = head; chunk; chunk = chunk->next){
            result.append(reinterpret_cast<const char*>(chunk->page_ptr + chunk->sent),
                          chunk->written - chunk->sent);
        }
        return result;
    }
#endif

    OutputBuffer(const OutputBuffer&) = delete;
    OutputBuffer& operator=(const OutputBuffer&) = delete;
    OutputBuffer(OutputBuffer&&) = delete;
    OutputBuffer& operator=(OutputBuffer&&) = delete;
};
