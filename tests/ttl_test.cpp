#include "../src/command/dispatcher.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <thread>

std::string execute(const std::vector<std::string>& args) {
    std::string input = "*" + std::to_string(args.size()) + "\r\n";
    for(const auto& arg : args){
        input += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    }
    RespParser parser;
    ByteRange request(input.data(), input.size());
    assert(parser.parse(request) == ParseStatus::Complete);
    OutputBuffer output;
    ReplyWriter reply(output);
    assert(dispatch(request, parser.arguments(), reply));
    return output.flatten_for_test();
}

void check_heap() {
    ExpireHeap heap;
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<std::optional<std::int64_t>> expected(32);
    for(std::size_t i = 0; i < expected.size(); ++i){
        entries.push_back(std::make_unique<Entry>(std::to_string(i), std::string("v")));
    }
    std::mt19937 random(20261108);
    for(int step = 0; step < 5000; ++step){
        const auto index = random() % entries.size();
        if(random() % 3 == 0){
            assert(heap.remove(entries[index].get()) == expected[index].has_value());
            expected[index].reset();
        }else{
            expected[index] = random() % 1000;
            heap.set(entries[index].get(), *expected[index]);
        }
        std::optional<std::int64_t> minimum;
        std::size_t count = 0;
        for(const auto& value : expected) if(value) ++count;
        std::vector<bool> seen(count);
        for(std::size_t i = 0; i < entries.size(); ++i){
            assert(heap.get_expire_time(entries[i].get()) == expected[i]);
            if(expected[i]){
                const auto position = entries[i]->heap_idx;
                assert(position < count && !seen[position]);
                seen[position] = true;
                if(!minimum || *expected[i] < *minimum) minimum = expected[i];
            }else{
                assert(entries[i]->heap_idx == NO_HEAP_INDEX);
            }
        }
        assert((heap.top() != nullptr) == minimum.has_value());
        if(minimum) assert(heap.top()->expires_time == *minimum);
    }
    while(const auto* item = heap.top()) assert(heap.remove(item->map_entry));
    for(const auto& entry : entries) assert(entry->heap_idx == NO_HEAP_INDEX);
}

void check_database() {
    class Database db;
    assert(db.pttl("missing") == -2);
    assert(!db.persist("missing"));
    assert(!db.pexpire("missing", 100).value);
    assert(db.set("key", "value", 60000) == RequestStatus::OK);
    assert(db.pttl("key") > 0 && db.pttl("key") <= 60000);
    assert(db.set("key", "bad", 0) == RequestStatus::InvalidExpire);
    assert(db.set("key", "bad", -1) == RequestStatus::InvalidExpire);
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    assert(db.set("key", "bad", maximum) == RequestStatus::InvalidExpire);
    assert(db.pexpire("key", maximum).status == RequestStatus::InvalidExpire);
    assert(*db.get("key").value == "value");
    assert(db.pttl("key") > 0);
    assert(db.pexpire("key", 120000).value);
    assert(db.pttl("key") > 60000);
    assert(db.persist("key"));
    assert(!db.persist("key"));
    assert(db.pttl("key") == -1);
    assert(db.pexpire("key", 60000).value);
    assert(db.set("key", "replacement") == RequestStatus::OK);
    assert(db.pttl("key") == -1);
    assert(db.pexpire("key", 0).value);
    assert(db.pttl("key") == -2);
    db.set("key", "v");
    assert(db.pexpire("key", std::numeric_limits<std::int64_t>::min()).value);

    db.zadd("zset", {{"member", 1.0}});
    assert(db.pexpire("zset", 60000).value);
    db.zadd("zset", {{"second", 2.0}});
    assert(db.pttl("zset") > 0);
    db.zrem("zset", {"member", "second"});
    assert(db.pttl("zset") == -2);
    db.zadd("zset", {{"member", 1.0}});
    db.pexpire("zset", 60000);
    db.set("zset", "string", 60000);
    assert(*db.get("zset").value == "string");
    assert(db.del("zset"));

    // Each command must discover expiration even before active cleanup runs.
    for(const auto* key : {"get", "del", "pttl", "persist", "pexpire", "zadd", "set"}){
        db.set(key, "v", 1);
    }
    for(const auto* key : {"zscore", "zrank", "zrange", "zrem"}){
        db.zadd(key, {{"member", 1.0}});
        db.pexpire(key, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(db.get("get").status == RequestStatus::Missing);
    assert(!db.del("del"));
    assert(db.pttl("pttl") == -2);
    assert(!db.persist("persist"));
    assert(!db.pexpire("pexpire", 60000).value);
    assert(db.zadd("zadd", {{"new", 1.0}}).value == 1);
    assert(db.pttl("zadd") == -1);
    assert(db.set("set", "new") == RequestStatus::OK);
    assert(db.pttl("set") == -1);
    assert(db.zscore("zscore", "member").status == RequestStatus::Missing);
    assert(db.zrank("zrank", "member").status == RequestStatus::Missing);
    assert(db.zrange("zrange", 0, -1).value.empty());
    assert(db.zrem("zrem", {"member"}).value == 0);

    class Database batch;
    for(int i = 0; i < 130; ++i) batch.set(std::to_string(i), "v", 60000);
    batch.set("permanent", "v");
    assert(batch.expire_due(monotonic_ms()) == 0);
    assert(batch.expire_due(maximum, 0) == 0);
    assert(batch.expire_due(maximum) == 64);
    assert(batch.expire_due(maximum) == 64);
    assert(batch.expire_due(maximum) == 2);
    assert(batch.expire_due(maximum) == 0);
    assert(batch.get("permanent").status == RequestStatus::OK);
    // Leave a live TTL entry to exercise destruction order.
    batch.set("destructor", "v", 60000);
}

void check_dispatcher() {
    const auto check = [](const std::vector<std::string>& args, const std::string& expected) {
        assert(execute(args) == expected);
    };
    const std::string arity = "-ERR wrong number of arguments\r\n";
    const std::string integer_error = "-ERR value is not an integer or out of range\r\n";
    const std::string set_expire_error = "-ERR invalid expire time in 'set' command\r\n";
    check({"PTTL", "absent"}, ":-2\r\n");
    check({"PEXPIRE", "absent", "1000"}, ":0\r\n");
    check({"PERSIST", "absent"}, ":0\r\n");
    check({"sEt", "ttl", "value", "pX", "60000"}, "+OK\r\n");
    const auto remaining = execute({"pTtL", "ttl"});
    assert(remaining.front() == ':' && remaining.substr(remaining.size() - 2) == "\r\n");
    const auto ttl = std::stoll(remaining.substr(1));
    assert(ttl > 0 && ttl <= 60000);
    for(const auto* invalid : {"", "1x", "1.5", "+1", "01", "-0", " 1", "9223372036854775808"}){
        check({"SET", "ttl", "changed", "PX", invalid}, integer_error);
        check({"PEXPIRE", "ttl", invalid}, integer_error);
    }
    for(const auto* invalid : {"0", "-1", "9223372036854775807"}){
        check({"SET", "ttl", "changed", "PX", invalid}, set_expire_error);
    }
    check({"PEXPIRE", "ttl", "9223372036854775807"},
          "-ERR invalid expire time in 'pexpire' command\r\n");
    check({"SET", "ttl", "changed", "EX", "1"}, "-ERR syntax error\r\n");
    check({"GET", "ttl"}, "$5\r\nvalue\r\n");
    assert(Database.pttl("ttl") > 0);
    for(const auto& args : std::vector<std::vector<std::string>>{
            {"SET", "ttl", "v", "PX"}, {"PEXPIRE", "ttl"},
            {"PEXPIRE", "ttl", "100", "NX"}, {"PTTL"},
            {"PTTL", "ttl", "extra"}, {"PERSIST"}, {"PERSIST", "ttl", "extra"}}){
        check(args, arity);
    }
    check({"pExPiRe", "ttl", "30000"}, ":1\r\n");
    check({"pErSiSt", "ttl"}, ":1\r\n");
    check({"PERSIST", "ttl"}, ":0\r\n");
    check({"PTTL", "ttl"}, ":-1\r\n");
    check({"PEXPIRE", "ttl", "-1"}, ":1\r\n");
    check({"GET", "ttl"}, "$-1\r\n");
    check({"SET", "ttl", "v", "PX", "1"}, "+OK\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check({"GET", "ttl"}, "$-1\r\n");
    check({"PTTL", "ttl"}, ":-2\r\n");
    const std::string binary("k\0v", 3);
    check({"SET", binary, binary, "PX", "60000"}, "+OK\r\n");
    check({"GET", binary}, "$3\r\n" + binary + "\r\n");
    check({"DEL", binary}, ":1\r\n");
}

int main() {
    chunk_pool.initialize();
    check_heap();
    check_database();
    check_dispatcher();
    std::cout << "TTL heap, database and RESP command checks passed\n";
}
