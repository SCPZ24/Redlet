#pragma once

#include <unistd.h>

class Connection{
private:
    int fd;

public:
    Connection(int client_fd): fd(client_fd){}

    

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ~Connection(){
        close(fd);
    }
};
