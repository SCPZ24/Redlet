#include "../src/net/connection.hpp"

#include <cassert>
#include <iostream>

int main() {
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
    std::cout << "Connection send and half-close checks passed\n";
}
