#pragma once

#include "../config.hpp"
#include "../storage/database.hpp"
#include "../protocol/reply_writer.hpp"
#include "../buffer/byte_range.hpp"
#include "../protocol/resp_parser.hpp"

#include <vector>
#include <string_view>


bool inline dispatch(ByteRange request, const std::vector<ArgumentRange>& arguments, const ReplyWriter& reply){
    // Called only for a complete RESP request; argument 0 is the command.
    const auto argument = [&](std::size_t index) {
        const auto& range = arguments[index];
        return std::string_view(request.data() + range.offset, range.length);
    };
    const auto command = argument(0);
    const auto matches = [&](std::string_view name) {
        if(command.size() != name.size()) return false;
        for(std::size_t i = 0; i < name.size(); ++i){
            char ch = command[i];
            if(ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            if(ch != name[i]) return false;
        }
        return true;
    };

    if(matches("PING")){
        if(arguments.size() == 1) return reply.raw_str("PONG", 4);
        if(arguments.size() != 2) return reply.error(arity_error, sizeof(arity_error) - 1);
        const auto message = argument(1);
        return reply.bulk_str(message.data(), message.size());
    }
    if(matches("GET")){
        if(arguments.size() != 2) return reply.error(arity_error, sizeof(arity_error) - 1);
        std::string key(argument(1));
        const auto* value = Database.get(key);
        return value ? reply.bulk_str(value->data(), value->size()) : reply.null_bulk();
    }
    if(matches("SET")){
        if(arguments.size() != 3) return reply.error(arity_error, sizeof(arity_error) - 1);
        Database.set(std::string(argument(1)), std::string(argument(2)));
        return reply.raw_str("OK", 2);
    }
    if(matches("DEL")){
        if(arguments.size() < 2) return reply.error(arity_error, sizeof(arity_error) - 1);
        std::int64_t deleted = 0;
        for(std::size_t i = 1; i < arguments.size(); ++i){
            std::string key(argument(i));
            deleted += Database.del(key);
        }
        return reply.integer(deleted);
    }
    return reply.error("ERR unknown command", 19);
}
