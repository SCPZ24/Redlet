#pragma once

#include "config.hpp"
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>


#include <unordered_map>


#include "net/connection.hpp"
#include "net/event_loop.hpp"


class Server{
private:
    int listen_fd;
    sockaddr_in sock_addr;
    EventLoop event_loop;
    std::unordered_map<int, Connection> connection_map;


    void inline add_connection(int client_fd){
        connection_map.try_emplace(client_fd,client_fd);
        event_loop.add(client_fd, EPOLLIN | EPOLLRDHUP);
    }

    void inline delete_connection(int client_fd){
        auto it = connection_map.find(client_fd);

        if(it == connection_map.end()) return;

        event_loop.remove(client_fd);
        connection_map.erase(client_fd);
    }

public:
    Server(unsigned short _port = LISTENTING_PORT){
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
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

    }

    ~Server() {
        close(listen_fd);
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};
