#pragma once

#include "../config.hpp"

#include <cerrno>
#include <cstddef>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <string>

class Connection{
private:
    int fd;
    bool want_read;
    bool want_write;

    std::string incoming;
    std::size_t consumed_pos = 0;

public:
    Connection(int client_fd): fd(client_fd){
        want_read = true;
        want_write = false;
    }

    int get_fd()const{
        return fd;
    }


    int read_buffer(){
        char chunk[INPUT_BUFFER_SIZE];

        ssize_t rd;
        do{
            rd = read(fd, chunk, sizeof(chunk));
        }while(rd < 0 && errno == EINTR);

        if(rd < 0){
            const int err = errno;
            if(err == EAGAIN || err == EWOULDBLOCK){
                want_read = true;
                return 0;
            }
        }else if(rd == 0){
            want_read = false;
        }else{
            incoming.append(chunk, static_cast<size_t>(rd));
        }

        return 0;
    }


    int write_buffer(){
        return -1;
    }






    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ~Connection(){
        close(fd);
    }
};
