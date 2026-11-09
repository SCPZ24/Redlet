#define main redlet_main
#include "src/main.cpp"
#undef main
#include <cstdio>
#include <cerrno>
#include <sys/mman.h>

static unsigned long long move_calls = 0, move_bytes = 0;
extern "C" void* __real_memmove(void*, const void*, size_t);
extern "C" void* __wrap_memmove(void* dst, const void* src, size_t size) {
    ++move_calls; move_bytes += size;
    return __real_memmove(dst, src, size);
}

static unsigned long long sends = 0, submitted_bytes = 0, accepted_bytes = 0, eagain = 0, short_writes = 0;
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __real_sendmsg(int, const msghdr*, int);
extern "C" int __real_epoll_wait(int, epoll_event*, int, int);

ssize_t record_send(ssize_t result, size_t submitted) {
    ++sends;
    submitted_bytes += submitted;
    if(result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ++eagain;
    if(result > 0) {
        accepted_bytes += result;
        if(static_cast<size_t>(result) < submitted) ++short_writes;
    }
    return result;
}
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    return record_send(__real_send(fd, data, size, flags), size);
}
extern "C" ssize_t __wrap_sendmsg(int fd, const msghdr* message, int flags) {
    size_t bytes = 0;
    for(size_t i = 0; i < message->msg_iovlen; ++i) bytes += message->msg_iov[i].iov_len;
    return record_send(__real_sendmsg(fd, message, flags), bytes);
}
extern "C" int __wrap_epoll_wait(int fd, epoll_event* events, int max_events, int timeout) {
    static std::int64_t last = 0;
    const auto now = monotonic_ms();
    if(now - last >= 200) {
        last = now;
        std::fprintf(stderr, "DIAG t=%lld sends=%llu offered=%llu accepted=%llu eagain=%llu short=%llu",
                     static_cast<long long>(now), sends, submitted_bytes, accepted_bytes, eagain, short_writes);
#if POOL_VERSION
        const auto s = chunk_pool.stats();
        std::fprintf(stderr, " mapped=%zu inuse=%zu warm=%zu cold=%zu growth=%llu advice=%llu unmap=%llu maintenance=%llu total_us=%llu max_us=%llu budgets=%llu",
                     s.mapped_bytes, s.in_use_chunks, s.warm_free_chunks, s.cold_free_chunks,
                     static_cast<unsigned long long>(s.growth_count), static_cast<unsigned long long>(s.advice_calls),
                     static_cast<unsigned long long>(s.unmap_calls), static_cast<unsigned long long>(s.maintenance_calls),
                     static_cast<unsigned long long>(s.maintenance_total_us), static_cast<unsigned long long>(s.maintenance_max_us),
                     static_cast<unsigned long long>(s.budget_exhaustions));
#endif
#ifdef TRACK_OUTPUT_MOVES
        std::fprintf(stderr, " output_moved_bytes=%llu", static_cast<unsigned long long>(output_moved_bytes));
#endif
        std::fprintf(stderr, " moves=%llu move_bytes=%llu\n", move_calls, move_bytes);
    }
    return __real_epoll_wait(fd, events, max_events, timeout);
}
int main() { return redlet_main(); }
