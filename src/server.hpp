#pragma once

#include "config.hpp"
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>


#include <unordered_map>


#include "net/connection.hpp"
#include "net/event_loop.hpp"
#include "net/idle_queue.hpp"
#include "util.hpp"


class Server{
private:
    int listen_fd;
    sockaddr_in sock_addr;
    EventLoop event_loop;
    std::unordered_map<int, Connection> connection_map;
    IdleQueue idle_queue;


    void inline add_connection(int client_fd){
        auto [it, inserted] = connection_map.try_emplace(client_fd, client_fd);
        idle_queue.add(&it->second);
        event_loop.add(client_fd, EPOLLIN | EPOLLRDHUP);
    }

    void inline delete_connection(int client_fd){
        idle_queue.unlink(&connection_map.at(client_fd));
        event_loop.remove(client_fd);
        connection_map.erase(client_fd);
    }

    void inline set_nonblock(int fd){
        const int flags = fcntl(fd, F_GETFL, 0);
        if(flags == -1){
            throw std::system_error(errno, std::generic_category(), "fcntl F_GETFL");
        }
        if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1){
            throw std::system_error(errno, std::generic_category(), "fcntl F_SETFL");
        }
    }

    void inline accept_connections(){
        while(true){
            const int client_fd = accept(listen_fd, nullptr, nullptr);
            if(client_fd >= 0){
                const int nodelay = 1;
                if(setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == -1){
                    const int error = errno;
                    close(client_fd);
                    throw std::system_error(error, std::generic_category(), "setsockopt TCP_NODELAY");
                }
                set_nonblock(client_fd);
                add_connection(client_fd);
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                return;
            }
            if(errno == EINTR || errno == ECONNABORTED){
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "accept");
        }
    }

public:
    Server(unsigned short _port = LISTENTING_PORT){
        listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if(listen_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket");
        }

        const int val = 1;
        if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == -1) {
            const int error = errno;
            ::close(listen_fd);
            throw std::system_error(error, std::generic_category(), "setsockopt");
        }

        sock_addr = {};
        sock_addr.sin_family = AF_INET;
        sock_addr.sin_port = htons(_port);
        sock_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if(bind(listen_fd, reinterpret_cast<sockaddr*>(&sock_addr), sizeof(sock_addr)) == -1){
            const int error = errno;
            close(listen_fd);
            throw std::system_error(error, std::generic_category(), "bind");
        }
    }

    void run(){
        if(listen(listen_fd, SOMAXCONN) < 0){
            const int error = errno;
            throw std::system_error(error, std::generic_category(), "listen");
        }
        event_loop.add(listen_fd, EPOLLIN);

        while(true){
            const auto until_maintenance = std::max(std::int64_t{0},
                chunk_pool.next_maintenance_ms() - monotonic_ms());
            const auto wait_ms = std::min(static_cast<std::int64_t>(DEFAULT_WAKE_DURATION), until_maintenance);
            const int num_events = event_loop.wait(static_cast<int>(wait_ms));
            const auto& ready_events = event_loop.get_ready_events();

            std::int64_t now = monotonic_ms();
            Database.expire_due(now);

            for(int i = 0; i < num_events; ++i){
                const int cur_fd = ready_events[i].data.fd;
                const uint32_t flag = ready_events[i].events;
                
                if(cur_fd == listen_fd){
                    if(flag & EPOLLIN){
                        accept_connections();
                    }
                }else{
                    Connection& cur_conn = connection_map.find(cur_fd)->second;

                    idle_queue.reset(&cur_conn);

                    if(flag & EPOLLERR){
                        int socket_error = 0;
                        socklen_t error_length = sizeof(socket_error);
                        if(getsockopt(cur_conn.get_fd(), SOL_SOCKET, SO_ERROR,
                                      &socket_error, &error_length) == -1){
                            throw std::system_error(errno, std::generic_category(), "getsockopt SO_ERROR");
                        }else if(socket_error != 0){
                            std::cerr << "socket fd=" << cur_fd << " error=" << socket_error << '\n';
                        }else{
                            std::cerr << "EPOLLERR fd=" << cur_fd
                                      << ": no pending SO_ERROR\n";
                        }
                        delete_connection(cur_fd);
                        continue;
                    }

                    if((flag & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) && cur_conn.ready_to_read()){
                        const int err = cur_conn.read_buffer();
                        if(err != 0){
                            std::cerr<< "read fd:" << cur_fd << "error" << std::endl;
                            delete_connection(cur_fd);
                            continue;
                        }
                    }

                    if((flag & (EPOLLOUT | EPOLLHUP)) && cur_conn.ready_to_write()){
                        const int err = cur_conn.write_buffer();
                        if(err != 0){
                            std::cerr<< "write fd:" << cur_fd << "error" << std::endl;
                            delete_connection(cur_fd);
                            continue;
                        }
                    }

                    if(cur_conn.ready_to_close()){
                        delete_connection(cur_fd);
                        continue;
                    }

                    uint32_t events = 0;
                    if(cur_conn.ready_to_read()){
                        events |= EPOLLIN | EPOLLRDHUP;
                    }
                    if(cur_conn.ready_to_write()){
                        events |= EPOLLOUT;
                    }
                    event_loop.modify(cur_fd, events);
                }
            }

            now = monotonic_ms();
            while(Connection* timeout_conn = idle_queue.takle_timeout(now)){
                delete_connection(timeout_conn->get_fd());
            }
            now = monotonic_ms();
            if(now >= chunk_pool.next_maintenance_ms()) chunk_pool.maintain(now);
        }
    }

    ~Server() {
        close(listen_fd);
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};
