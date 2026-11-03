#pragma once

#include "../config.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <system_error>
#include <sys/epoll.h>
#include <unistd.h>



class EventLoop{
private:
    int epoll_fd;
    std::array<epoll_event, MAX_EPOLL_EVENTS> ready_events;


public:
    EventLoop(){
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if(epoll_fd == -1){
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
    }

    void add(int fd, uint32_t events){
        epoll_event event{};
        event.data.fd = fd;
        event.events = events;

        if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1){
            throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
        }
    }

    void modify(int fd, uint32_t events){
        epoll_event event{};
        event.data.fd = fd;
        event.events = events;

        if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1){
            throw std::system_error(errno, std::generic_category(), "epoll_ctl MOD");
        }
    }

    void remove(int fd){
        if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1){
            throw std::system_error(errno, std::generic_category(), "epoll_ctl DEL");
        }
    }

    int wait(int timeout_ms = -1){
        int wait = epoll_wait(epoll_fd, ready_events.data(), MAX_EPOLL_EVENTS, timeout_ms);
        if(wait == -1){
            throw std::system_error(errno, std::system_category(), "epoll_wait");
        }

        return wait;
    }

    const std::array<epoll_event, MAX_EPOLL_EVENTS>& get_ready_events() const{
        return ready_events;
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    ~EventLoop(){
        close(epoll_fd);
    }
};