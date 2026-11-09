#include "../src/server.hpp"

#include <cassert>
#include <iostream>

static int waits = 0;
extern "C" int __real_epoll_wait(int, epoll_event*, int, int);
extern "C" int __wrap_epoll_wait(int fd, epoll_event* events, int max_events, int timeout) {
    assert(timeout >= 0 && timeout <= 1);
    // Let the real idle wait finish, then stop the otherwise endless Server loop.
    if(++waits == 3){ errno = EBADF; return -1; }
    return __real_epoll_wait(fd, events, max_events, timeout);
}

int main() {
    PoolConfig config;
    config.maintenance_interval_ms = 1;
    config.warm_high_chunks = 0;
    config.warm_target_chunks = 0;
    config.warm_min_idle_ms = 0;
    config.empty_slab_idle_ms = 0;
    config.keep_empty_slabs = 0;
    config.maintenance_budget_us = 1000000;
    chunk_pool.initialize(config);
    auto* chunk = chunk_pool.try_acquire();
    assert(chunk);
    chunk_pool.release(chunk, monotonic_ms());
    bool stopped = false;
    {
        Server server(0);
        try { server.run(); }
        catch(const std::system_error& error) {
            assert(error.code().value() == EBADF);
            stopped = true;
        }
    }
    assert(stopped && waits == 3);
    assert(chunk_pool.stats().maintenance_calls >= 1);
    assert(chunk_pool.stats().mapped_bytes == 0);
    assert(chunk_pool.stats().in_use_chunks == 0);
    std::cout << "Server idle maintenance checks passed\n";
}
