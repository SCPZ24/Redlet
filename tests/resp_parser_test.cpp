#define test
#include "../src/protocol/resp_parser.hpp"
#include "../src/net/connection.hpp"
#undef test

#include <cassert>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>

ByteRange bytes(const std::string& value) {
    return ByteRange(value.data(), value.size());
}

std::string request(const std::vector<std::string>& args) {
    std::string result = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) {
        result += "$" + std::to_string(arg.size()) + "\r\n";
        result += arg;
        result += "\r\n";
    }
    return result;
}

void check_arguments(const RespParser& parser, const std::string& input,
                     const std::vector<std::string>& args) {
    assert(parser.status() == ParseStatus::Complete);
    assert(parser.request_size() == input.size());
    assert(parser.arguments().size() == args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& range = parser.arguments()[i];
        assert(input.substr(range.offset, range.length) == args[i]);
    }
}

void fragmentation() {
    const std::vector<std::string> args{"SET", "key", std::string("a\0b\r\nc", 6), ""};
    const auto input = request(args);
    for (std::size_t split = 0; split < input.size(); ++split) {
        RespParser parser;
        assert(parser.parse(ByteRange(input.data(), split)) == ParseStatus::NeedMore);
        assert(parser.parse(bytes(input)) == ParseStatus::Complete);
        check_arguments(parser, input, args);
    }
    RespParser parser;
    std::string growing;
    for (std::size_t i = 0; i < input.size(); ++i) {
        growing.push_back(input[i]);
        const auto status = parser.parse(bytes(growing));
        assert(status == (i + 1 == input.size() ? ParseStatus::Complete : ParseStatus::NeedMore));
    }
    check_arguments(parser, growing, args);
    parser.reset();
    const auto ping = request({"PING"});
    assert(parser.parse(bytes(ping)) == ParseStatus::Complete);
    check_arguments(parser, ping, {"PING"});
}

void invalid_inputs() {
    const std::vector<std::string> invalid{
        "PING\r\n", "*0\r\n", "*-1\r\n", "*+1\r\n", "*\r\n", "*1\nx",
        "*1\rX", "*1\r\n:1\r\n", "*1\r\n$-1\r\n", "*1\r\n$+1\r\n",
        "*1\r\n$\r\n", "*1\r\n$1\rX", "*1\r\n$1\r\nax",
        "*1\r\n$1\r\na\rX", "*999999999999999999999999999999\r\n",
        "*1\r\n$999999999999999999999999999999\r\n",
        "*" + std::to_string(MAX_REQUEST_ARGS + 1) + "\r\n",
        "*1\r\n$" + std::to_string(MAX_BULK_BYTES + 1) + "\r\n"
    };
    for (const auto& input : invalid) {
        RespParser parser;
        assert(parser.parse(bytes(input)) == ParseStatus::Invalid);
        assert(parser.parse(bytes(input)) == ParseStatus::Invalid);
    }
}

void limits() {
    RespParser parser;
    const auto large = request({std::string(MAX_BULK_BYTES, 'x')});
    for (std::size_t length = 1; length < large.size(); length += 997) {
        assert(parser.parse(ByteRange(large.data(), length)) == ParseStatus::NeedMore);
    }
    assert(parser.parse(bytes(large)) == ParseStatus::Complete);
    parser.reset();
    const auto many = request(std::vector<std::string>(MAX_REQUEST_ARGS, ""));
    assert(parser.parse(bytes(many)) == ParseStatus::Complete);
    parser.reset();
    // *2\r\n plus two seven-digit bulk headers and their trailers is 28 bytes.
    const auto exact = request({std::string(MAX_BULK_BYTES, 'a'),
                               std::string(MAX_BULK_BYTES - 28, 'b')});
    assert(exact.size() == MAX_REQUEST_BYTES);
    assert(parser.parse(bytes(exact)) == ParseStatus::Complete);
    parser.reset();
    const auto too_big = request({std::string(MAX_BULK_BYTES, 'a'),
                                 std::string(MAX_BULK_BYTES - 27, 'b')});
    assert(parser.parse(bytes(too_big)) == ParseStatus::Invalid);
    parser.reset();
    const std::string oversized_header = "*" + std::string(MAX_REQUEST_BYTES, '0');
    assert(parser.parse(bytes(oversized_header)) == ParseStatus::Invalid);
}

void connection_pipeline() {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    Connection conn(sockets[0]);
    assert(conn.read_buffer() == 0); // EAGAIN
    assert(conn.ready_to_read());
    const auto first = request({"PING"});
    const auto second = request({"GET", "key"});
    const auto input = first + second;
    assert(write(sockets[1], input.data(), input.size()) == static_cast<ssize_t>(input.size()));
    assert(conn.read_buffer() == 0);
    assert(conn.pending_output() == "+PONG\r\n$-1\r\n");
    close(sockets[1]);
    assert(conn.read_buffer() == 0);
    assert(!conn.ready_to_read());
}

void connection_fragmentation() {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    Connection conn(sockets[0]);
    const auto input = request({"PING", std::string(INPUT_BUFFER_SIZE * 3, 'x')});
    for (char byte : input) {
        assert(write(sockets[1], &byte, 1) == 1);
        assert(conn.read_buffer() == 0);
    }
    assert(conn.pending_output() == "$" + std::to_string(INPUT_BUFFER_SIZE * 3) + "\r\n"
           + std::string(INPUT_BUFFER_SIZE * 3, 'x') + "\r\n");
    const std::string partial = "*1\r\n$5\r\nab";
    assert(write(sockets[1], partial.data(), partial.size()) == static_cast<ssize_t>(partial.size()));
    assert(conn.read_buffer() == 0);
    assert(conn.ready_to_read());
    close(sockets[1]);
    assert(conn.read_buffer() == 0); // EOF only stops further reads.
    assert(!conn.ready_to_read());
}

void buffered_partial_successor() {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    Connection conn(sockets[0]);
    const auto next = request({"SET", "key", std::string("a\0b", 3)});
    const auto first = request({"PING"}) + next.substr(0, 8);
    assert(write(sockets[1], first.data(), first.size()) == static_cast<ssize_t>(first.size()));
    assert(conn.read_buffer() == 0);
    // The next read compacts the consumed prefix while keeping relative parser offsets.
    const auto tail = next.substr(8);
    assert(write(sockets[1], tail.data(), tail.size()) == static_cast<ssize_t>(tail.size()));
    assert(conn.read_buffer() == 0);
    assert(conn.pending_output() == "+PONG\r\n+OK\r\n");
    assert(write(sockets[1], "!", 1) == 1);
    assert(conn.read_buffer() == EPROTO);
    close(sockets[1]);
}

int main() {
    fragmentation();
    invalid_inputs();
    limits();
    connection_pipeline();
    connection_fragmentation();
    buffered_partial_successor();
    std::cout << "RESP parser and Connection checks passed\n";
}
