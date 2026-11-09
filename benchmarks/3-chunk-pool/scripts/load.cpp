#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
struct Connection {
    int fd;
    std::size_t sent = 0, received = 0, batches;
    Clock::time_point started;
};
struct Worker {
    std::vector<double> latency;
    std::exception_ptr error;
};

void run_worker(int index, int clients, std::size_t batches, const std::string& request,
                const std::string& expected, std::atomic<int>& ready, std::atomic<bool>& start,
                Worker& result) {
    std::vector<Connection> connections;
    int epoll = -1;
    try {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(2 + index, &mask);
        if(sched_setaffinity(0, sizeof(mask), &mask) != 0) throw std::runtime_error("affinity");
        epoll = epoll_create1(0);
        if(epoll < 0) throw std::runtime_error("epoll_create1");
        for(int i = 0; i < clients; ++i) {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if(fd < 0) throw std::runtime_error("socket");
            connections.push_back({fd, 0, 0, batches, {}});
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(6379);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) throw std::runtime_error("connect");
            const int yes = 1;
            if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) throw std::runtime_error("TCP_NODELAY");
            if(fcntl(fd, F_SETFL, O_NONBLOCK) != 0) throw std::runtime_error("nonblock");
            epoll_event event{};
            event.data.u32 = i;
            event.events = EPOLLIN | EPOLLOUT;
            if(epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event) != 0) throw std::runtime_error("epoll add");
        }
        result.latency.reserve(batches * clients);
        ++ready;
        while(!start.load()) std::this_thread::yield();
        int alive = clients;
        std::array<epoll_event, 256> events;
        std::array<char, 65536> buffer;
        while(alive) {
            const int count = epoll_wait(epoll, events.data(), events.size(), 30000);
            if(count <= 0) throw std::runtime_error("epoll timeout/error");
            for(int i = 0; i < count; ++i) {
                auto& c = connections.at(events[i].data.u32);
                if(events[i].events & (EPOLLERR | EPOLLHUP)) throw std::runtime_error("socket error/hangup");
                if(events[i].events & EPOLLOUT) {
                    if(c.sent == 0) c.started = Clock::now();
                    const auto sent = send(c.fd, request.data() + c.sent, request.size() - c.sent, MSG_NOSIGNAL);
                    if(sent < 0 && errno != EAGAIN && errno != EINTR) throw std::runtime_error("send");
                    if(sent > 0) c.sent += sent;
                    if(c.sent == request.size()) {
                        epoll_event event{};
                        event.data.u32 = events[i].data.u32;
                        event.events = EPOLLIN;
                        if(epoll_ctl(epoll, EPOLL_CTL_MOD, c.fd, &event) != 0) throw std::runtime_error("epoll read");
                    }
                }
                if(events[i].events & EPOLLIN) {
                    const auto received = recv(c.fd, buffer.data(), std::min(buffer.size(), expected.size() - c.received), 0);
                    if(received == 0 || (received < 0 && errno != EAGAIN && errno != EINTR)) throw std::runtime_error("recv/EOF");
                    if(received > 0) {
#ifdef CLIENT_QUICKACK
                        const int quickack = 1;
                        if(setsockopt(c.fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack)) != 0) throw std::runtime_error("TCP_QUICKACK");
#endif
                        if(std::memcmp(buffer.data(), expected.data() + c.received, received) != 0) throw std::runtime_error("response mismatch");
                        c.received += received;
                    }
                    if(c.received == expected.size()) {
                        result.latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - c.started).count());
                        if(--c.batches == 0) {
                            if(epoll_ctl(epoll, EPOLL_CTL_DEL, c.fd, nullptr) != 0) throw std::runtime_error("epoll delete");
                            shutdown(c.fd, SHUT_WR);
                            close(c.fd);
                            c.fd = -1;
                            --alive;
                        } else {
                            c.sent = c.received = 0;
                            epoll_event event{};
                            event.data.u32 = events[i].data.u32;
                            event.events = EPOLLIN | EPOLLOUT;
                            if(epoll_ctl(epoll, EPOLL_CTL_MOD, c.fd, &event) != 0) throw std::runtime_error("epoll write");
                        }
                    }
                }
            }
        }
    } catch(...) {
        result.error = std::current_exception();
        ++ready;
    }
    for(auto& c : connections) if(c.fd >= 0) close(c.fd);
    if(epoll >= 0) close(epoll);
}

int main(int argc, char** argv) {
    try {
        if(argc != 5) throw std::runtime_error("usage: load key clients pipeline requests");
        const std::string key = argv[1];
        const int clients = std::stoi(argv[2]);
        const int pipeline = std::stoi(argv[3]);
        const auto requests = std::stoull(argv[4]);
        if(clients <= 0 || pipeline <= 0 || requests % (clients * pipeline) != 0) throw std::runtime_error("invalid request count");
        const std::size_t size = key == "small" ? 64 : key == "medium" ? 65536 : 1048576;
        const std::string get = "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
        const std::string reply = "$" + std::to_string(size) + "\r\n" + std::string(size, 'x') + "\r\n";
        std::string request, expected;
        for(int i = 0; i < pipeline; ++i) { request += get; expected += reply; }
        const int workers = std::min(4, clients);
        std::vector<Worker> results(workers);
        std::vector<std::thread> threads;
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        for(int i = 0; i < workers; ++i) {
            const int local = clients / workers + (i < clients % workers);
            threads.emplace_back(run_worker, i, local, requests / (clients * pipeline),
                                 std::cref(request), std::cref(expected), std::ref(ready), std::ref(start), std::ref(results[i]));
        }
        while(ready < workers) std::this_thread::yield();
        const auto begin = Clock::now();
        start = true;
        for(auto& thread : threads) thread.join();
        const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        std::vector<double> latencies;
        for(auto& result : results) {
            if(result.error) std::rethrow_exception(result.error);
            latencies.insert(latencies.end(), result.latency.begin(), result.latency.end());
        }
        if(latencies.size() != requests / pipeline) throw std::runtime_error("incomplete batches");
        std::sort(latencies.begin(), latencies.end());
        const auto percentile = [&](double p) { return latencies[static_cast<std::size_t>((latencies.size() - 1) * p)]; };
        std::cout << "test,rps,avg_latency_ms,min_latency_ms,p50_latency_ms,p95_latency_ms,p99_latency_ms,max_latency_ms\n";
        std::cout << std::fixed << std::setprecision(6) << "GET " << key << ',' << requests / seconds << ','
                  << std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size() << ','
                  << latencies.front() << ',' << percentile(.5) << ',' << percentile(.95) << ','
                  << percentile(.99) << ',' << latencies.back() << '\n';
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
