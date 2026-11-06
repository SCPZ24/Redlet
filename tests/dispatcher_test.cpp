#include "../src/command/dispatcher.hpp"

#include <cassert>
#include <iostream>

bool execute(const std::vector<std::string>& args, std::string& output) {
    std::string input = "*" + std::to_string(args.size()) + "\r\n";
    for(const auto& arg : args){
        input += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    }
    RespParser parser;
    ByteRange request(input.data(), input.size());
    assert(parser.parse(request) == ParseStatus::Complete);
    ReplyWriter reply(output);
    return dispatch(request, parser.arguments(), reply);
}

int main() {
    std::string output;
    const auto check = [&](const std::vector<std::string>& args, const std::string& expected) {
        output.clear();
        assert(execute(args, output));
        assert(output == expected);
    };
    check({"pInG"}, "+PONG\r\n");
    const std::string binary("a\0b\r\n", 5);
    const auto bulk = "$5\r\n" + binary + "\r\n";
    check({"PING", binary}, bulk);
    check({"PING", ""}, "$0\r\n\r\n");
    check({"GET", "missing"}, "$-1\r\n");
    check({"sEt", binary, binary}, "+OK\r\n");
    check({"gEt", binary}, bulk);
    check({"SET", binary, ""}, "+OK\r\n");
    check({"GET", binary}, "$0\r\n\r\n");
    check({"SET", "", "empty key"}, "+OK\r\n");
    check({"GET", ""}, "$9\r\nempty key\r\n");

    // Stored strings must outlive borrowed, non-null-terminated request slices.
    const std::string long_key = std::string(64, 'k') + binary;
    const std::string long_value = std::string(128, 'v') + binary;
    std::string backing = long_key + long_value + "trailer";
    Database.set(std::string_view(backing.data(), long_key.size()),
                 std::string_view(backing.data() + long_key.size(), long_value.size()));
    backing.assign(backing.size(), 'x');
    check({"GET", long_key}, "$" + std::to_string(long_value.size()) + "\r\n" + long_value + "\r\n");
    const std::string updated_value = std::string(192, 'u') + binary;
    backing = updated_value + "trailer";
    Database.set(long_key, std::string_view(backing.data(), updated_value.size()));
    backing.assign(backing.size(), 'x');
    check({"GET", long_key}, "$" + std::to_string(updated_value.size()) + "\r\n" + updated_value + "\r\n");
    check({"DEL", long_key}, ":1\r\n");

    const std::string arity = "-ERR wrong number of arguments\r\n";
    for(const auto& args : std::vector<std::vector<std::string>>{
            {"PING", "a", "b"}, {"GET"}, {"GET", "a", "b"},
            {"SET", binary}, {"SET", binary, "changed", "NX"}, {"DEL"}}){
        check(args, arity);
    }
    check({"GET", binary}, "$0\r\n\r\n"); // Invalid SET did not modify the value.
    check({std::string("GET\0", 4), binary}, "-ERR unknown command\r\n");
    check({"BAD\r\ncommand"}, "-ERR unknown command\r\n");
    check({"del", binary, binary, "", "missing"}, ":2\r\n");
    check({"GET", binary}, "$-1\r\n");

    output.clear();
    assert(execute({"unknown"}, output));
    assert(execute({"PING"}, output));
    assert(output == "-ERR unknown command\r\n+PONG\r\n");

    for(const auto& args : std::vector<std::vector<std::string>>{
            {"PING"}, {"PING", "x"}, {"GET", "missing"},
            {"SET", "committed", "yes"}, {"DEL", "missing"}, {"GET"}, {"unknown"}}){
        output.assign(MAX_OUTPUT_BYTES, 'x');
        const auto before = output;
        assert(!execute(args, output));
        assert(output == before);
    }
    // A successful database mutation is not rolled back on output exhaustion.
    check({"GET", "committed"}, "$3\r\nyes\r\n");

    std::cout << "Dispatcher checks passed\n";
}
