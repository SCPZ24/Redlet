#include "server.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        Server server;
        std::cout << "Redlet listening on 127.0.0.1:" << LISTENTING_PORT << std::endl;
        server.run();
    } catch(const std::exception& error) {
        std::cerr << "Redlet: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    
    return 0;
}
