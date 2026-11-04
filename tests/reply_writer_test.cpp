#include "../src/protocol/reply_writer.hpp"
#include "../src/net/connection.hpp"

#include <cassert>
#include <iostream>
#include <limits>

template<typename Encode>
void check_limit(std::size_t encoded_size, Encode encode) {
    std::string output(MAX_OUTPUT_BYTES - encoded_size, 'x');
    ReplyWriter reply(output);
    assert(encode(reply));
    assert(output.size() == MAX_OUTPUT_BYTES);

    output.assign(MAX_OUTPUT_BYTES - encoded_size + 1, 'x');
    const auto before = output;
    assert(!encode(reply));
    assert(output == before);
}

int main() {
    std::string output;
    ReplyWriter reply(output);
    assert(reply.raw_str("OK", 2));
    assert(reply.error("ERR unknown command", 19));
    assert(reply.integer(0));
    assert(reply.integer(std::numeric_limits<std::int64_t>::min()));
    assert(reply.integer(std::numeric_limits<std::int64_t>::max()));
    assert(reply.bulk_str("a\0b\r\n", 5));
    assert(reply.bulk_str("", 0));
    assert(reply.null_bulk());
    std::string expected = "+OK\r\n-ERR unknown command\r\n:0\r\n"
                           ":-9223372036854775808\r\n:9223372036854775807\r\n$5\r\n";
    expected.append("a\0b\r\n", 5);
    expected.append("\r\n$0\r\n\r\n$-1\r\n");
    assert(output == expected);

    check_limit(5, [](ReplyWriter& writer) { return writer.raw_str("OK", 2); });
    check_limit(6, [](ReplyWriter& writer) { return writer.error("ERR", 3); });
    check_limit(5, [](ReplyWriter& writer) { return writer.integer(-1); });
    check_limit(9, [](ReplyWriter& writer) { return writer.bulk_str("YES", 3); });
    check_limit(5, [](ReplyWriter& writer) { return writer.null_bulk(); });

    output.clear();
    // Reject impossible lengths before touching the supplied payload.
    assert(!reply.bulk_str("", std::numeric_limits<std::size_t>::max()));
    assert(!reply.raw_str("", std::numeric_limits<std::size_t>::max()));
    assert(!reply.error("", std::numeric_limits<std::size_t>::max()));
    assert(output.empty());

    const std::string large(MAX_BULK_BYTES, 'v');
    assert(reply.bulk_str(large.data(), large.size()));
    assert(output == "$" + std::to_string(large.size()) + "\r\n" + large + "\r\n");
    std::cout << "ReplyWriter checks passed\n";
}
