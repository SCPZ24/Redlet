#include "public.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <string>
using namespace std;

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    const int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
        perror("setsockopt");
        close(fd);
        return 1;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTENTING_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(fd, SOMAXCONN) < 0) {
        perror("bind/listen");
        close(fd);
        return 1;
    }

    while (true) {
        int conn_fd = accept(fd, nullptr, nullptr);
        if (conn_fd < 0) {
            perror("accept");
            close(fd);
            return 1;
        }
        string input;
        char buf[64];
        ssize_t n;
        while ((n = read(conn_fd, buf, sizeof(buf))) > 0) input.append(buf, n);
        if (n < 0) {
            perror("read");
            close(conn_fd);
            continue;
        }
        if (!input.empty() && input[0] == 'E') {
            close(conn_fd);
            break;
        }
        cout << input << " received" << endl;
        string response = input + " received";
        size_t sent = 0;
        while (sent < response.size()) {
            n = write(conn_fd, response.data() + sent, response.size() - sent);
            if (n <= 0) {
                perror("write");
                break;
            }
            sent += n;
        }
        close(conn_fd);
    }
    close(fd);
    return 0;
}
