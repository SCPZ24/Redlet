#include "../src/net/connection.hpp"

#include <cassert>
#include <iostream>

int main() {
    chunk_pool.initialize();
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    Connection conn(sockets[0]);
    const std::string payload(16384, 'x');
    const std::string request = "*2\r\n$4\r\nPING\r\n$" + std::to_string(payload.size())
                                + "\r\n" + payload + "\r\n";
    assert(send(sockets[1], request.data(), request.size(), MSG_NOSIGNAL)
           == static_cast<ssize_t>(request.size()));
    assert(shutdown(sockets[1], SHUT_WR) == 0);
    while(conn.ready_to_read()) assert(conn.read_buffer() == 0);
    assert(conn.ready_to_write());
    assert(!conn.ready_to_close());

    std::string received;
    char chunk[4096];
    for(int iterations = 0; conn.ready_to_write(); ++iterations){
        assert(iterations < 1000);
        assert(conn.write_buffer() == 0);
        ssize_t n;
        while((n = read(sockets[1], chunk, sizeof(chunk))) > 0){
            received.append(chunk, static_cast<std::size_t>(n));
        }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    assert(received == "$" + std::to_string(payload.size()) + "\r\n" + payload + "\r\n");
    assert(conn.ready_to_close());
    close(sockets[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    const int send_buffer = 4096;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    const std::string large(256 * 1024, 'v');
    Database.set("backpressure", large);
    const std::string get = "*2\r\n$3\r\nGET\r\n$12\r\nbackpressure\r\n";
    {
        Connection slow(sockets[0]);
        assert(send(sockets[1], get.data(), get.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(get.size()));
        assert(slow.read_buffer() == 0);
        bool blocked = false;
        for(int i = 0; i < 100; ++i){
            const auto before = slow.pending_output();
            assert(slow.write_buffer() == 0);
            if(slow.pending_output() == before){ blocked = true; break; }
        }
        assert(blocked && slow.ready_to_write());
        received.clear();
        for(int i = 0; slow.ready_to_write(); ++i){
            assert(i < 10000);
            assert(slow.write_buffer() == 0);
            ssize_t n;
            while((n = read(sockets[1], chunk, sizeof(chunk))) > 0){
                received.append(chunk, static_cast<std::size_t>(n));
            }
            assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        }
        assert(received == "$" + std::to_string(large.size()) + "\r\n" + large + "\r\n");
    }
    close(sockets[1]);
    assert(chunk_pool.stats().in_use_chunks == 0);
    std::cout << "Connection send, backpressure and half-close checks passed\n";
}
