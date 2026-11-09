#include "../src/net/connection.hpp"

#include <cassert>
#include <iostream>

static int send_error = 0;
static std::size_t send_limit = std::numeric_limits<std::size_t>::max();
static bool send_zero = false;
static std::string sent;
static std::size_t calls = 0;
extern "C" ssize_t __wrap_sendmsg(int, const msghdr* message, int flags) {
    ++calls;
    assert(flags == MSG_NOSIGNAL);
    std::size_t submitted = 0;
    for(std::size_t i = 0; i < message->msg_iovlen; ++i) submitted += message->msg_iov[i].iov_len;
    assert(submitted > 0 && submitted <= OUTPUT_BUFFER_SIZE);
    assert(message->msg_iovlen <= 32);
    if(send_error){
        errno = send_error;
        if(send_error == EINTR) send_error = 0;
        return -1;
    }
    if(send_zero) return 0;
    const auto accepted = std::min(submitted, send_limit);
    auto remaining = accepted;
    for(std::size_t i = 0; i < message->msg_iovlen && remaining; ++i){
        const auto& vec = message->msg_iov[i];
        const auto count = std::min(remaining, vec.iov_len);
        sent.append(static_cast<const char*>(vec.iov_base), count);
        remaining -= count;
    }
    return static_cast<ssize_t>(accepted);
}

int main() {
    PoolConfig config;
    config.max_mapped_bytes = static_cast<std::size_t>(sysconf(_SC_PAGESIZE)) * PageSlab::chunk_count;
    chunk_pool.initialize(config);
    const std::string value(100000, 'q');
    Database.set("large", value);
    const std::string request = "*2\r\n$3\r\nGET\r\n$5\r\nlarge\r\n";
    const std::string expected = "$100000\r\n" + value + "\r\n";
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    {
        Connection connection(sockets[0]);
        assert(send(sockets[1], request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        assert(connection.read_buffer() == 0);
        assert(connection.pending_output() == expected);
        send_error = EAGAIN;
        assert(connection.write_buffer() == 0);
        assert(connection.pending_output() == expected && connection.ready_to_write());
        send_error = 0;
        send_zero = true;
        assert(connection.write_buffer() == 0);
        assert(connection.pending_output() == expected);
        send_zero = false;
        send_error = EINTR;
        send_limit = 6000;
        const auto before_calls = calls;
        assert(connection.write_buffer() == 0);
        assert(calls == before_calls + 2);
        assert(sent == expected.substr(0, 6000));
        assert(connection.pending_output() == expected.substr(6000));
        assert(shutdown(sockets[1], SHUT_WR) == 0);
        assert(connection.read_buffer() == 0);
        assert(!connection.ready_to_close());
        while(connection.ready_to_write()) assert(connection.write_buffer() == 0);
        assert(sent == expected && connection.ready_to_close());
    }
    close(sockets[1]);
    assert(chunk_pool.stats().in_use_chunks == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    {
        Connection connection(sockets[0]);
        assert(send(sockets[1], request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        assert(connection.read_buffer() == 0);
        send_error = EPIPE;
        assert(connection.write_buffer() == EPIPE);
    }
    close(sockets[1]);
    assert(chunk_pool.stats().in_use_chunks == 0);
    send_error = 0;

    // Exhaustion partway through encoding must surface to the Server as ENOBUFS.
    std::vector<ChunkControlBlock*> held;
    while(chunk_pool.stats().in_use_chunks < PageSlab::chunk_count - 1){
        auto* chunk = chunk_pool.try_acquire();
        assert(chunk);
        held.push_back(chunk);
    }
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    {
        Connection connection(sockets[0]);
        assert(send(sockets[1], request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        const auto before_calls = calls;
        assert(connection.read_buffer() == ENOBUFS);
        assert(calls == before_calls);
    }
    close(sockets[1]);
    assert(chunk_pool.stats().in_use_chunks == held.size());
    for(auto* chunk : held) chunk_pool.release(chunk, monotonic_ms());
    assert(chunk_pool.stats().in_use_chunks == 0);
    std::cout << "Connection short-write and failure checks passed\n";
}
