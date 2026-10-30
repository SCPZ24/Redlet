#include "public.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <string>
using namespace std;

int main() {
    string input;
    while (cin >> input) {
        if (input[0] == 'R') break;
        if (input.size() > 63) {
            cerr << "Message is too long (maximum 63 bytes)" << endl;
            continue;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket"); return 1;
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(LISTENTING_PORT);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            perror("connect");
        close(fd);
        return 1;
        }

        // One connection carries one message. Closing the write side marks its end.
        size_t sent = 0;
        while (sent < input.size()) {
            ssize_t n = write(fd, input.data() + sent, input.size() - sent);
            if (n <= 0) {
                perror("write");
                close(fd);
                return 1;
            }
            sent += n;
        }
        shutdown(fd, SHUT_WR);
        
        if (input[0] == 'E') {
            close(fd);
            break;
        }

        char buf[128];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) cout.write(buf, n);
        if (n < 0) {
            perror("read");
            close(fd); return 1;
        }

        cout << " returned from server" << endl;
        close(fd);
    }
    return 0;
}
