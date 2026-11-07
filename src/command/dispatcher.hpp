#pragma once

#include "../config.hpp"
#include "../storage/database.hpp"
#include "../protocol/reply_writer.hpp"
#include "../buffer/byte_range.hpp"
#include "../protocol/resp_parser.hpp"

#include <vector>
#include <string_view>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>


bool inline dispatch(ByteRange request, const std::vector<ArgumentRange>& arguments, const ReplyWriter& reply){
    // Called only for a complete RESP request; argument 0 is the command.
    const auto argument = [&](std::size_t index) {
        const auto& range = arguments[index];
        return std::string_view(request.data() + range.offset, range.length);
    };
    const auto matches = [&](std::string_view name, std::size_t index = 0) {
        const auto command = argument(index);
        if(command.size() != name.size()) return false;
        for(std::size_t i = 0; i < name.size(); ++i){
            char ch = command[i];
            if(ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            if(ch != name[i]) return false;
        }
        return true;
    };
    constexpr char wrong_type[] = "WRONGTYPE Operation against a key holding the wrong kind of value";
    constexpr char invalid_score[] = "ERR value is not a valid float";
    const auto write_score = [&](double score) {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), score);
        if(result.ec != std::errc{}) return false;
        return reply.bulk_str(buffer, static_cast<std::size_t>(result.ptr - buffer));
    };

    if(matches("PING")){
        if(arguments.size() == 1) return reply.raw_str("PONG", 4);
        if(arguments.size() != 2) return reply.error(arity_error, sizeof(arity_error) - 1);
        const auto message = argument(1);
        return reply.bulk_str(message.data(), message.size());
    }
    if(matches("GET")){
        if(arguments.size() != 2) return reply.error(arity_error, sizeof(arity_error) - 1);
        const auto result = Database.get(argument(1));
        if(result.status == RequestStatus::Missing) return reply.null_bulk();
        if(result.status == RequestStatus::WrongType){
            return reply.error(wrong_type, sizeof(wrong_type) - 1);
        }
        return reply.bulk_str(result.value->data(), result.value->size());
    }
    if(matches("SET")){
        if(arguments.size() != 3) return reply.error(arity_error, sizeof(arity_error) - 1);
        Database.set(argument(1), argument(2));
        return reply.raw_str("OK", 2);
    }
    if(matches("DEL")){
        if(arguments.size() < 2) return reply.error(arity_error, sizeof(arity_error) - 1);
        std::int64_t deleted = 0;
        for(std::size_t i = 1; i < arguments.size(); ++i){
            deleted += Database.del(argument(i));
        }
        return reply.integer(deleted);
    }
    if(matches("ZADD")){
        if(arguments.size() < 4 || arguments.size() % 2 != 0){
            return reply.error(arity_error, sizeof(arity_error) - 1);
        }
        std::vector<Member> members;
        members.reserve((arguments.size() - 2) / 2);
        for(std::size_t i = 2; i < arguments.size(); i += 2){
            const std::string text(argument(i));
            char* end;
            errno = 0;
            const double score = std::strtod(text.c_str(), &end);
            if(text.empty() || std::isspace(static_cast<unsigned char>(text[0])) ||
               end != text.c_str() + text.size() || std::isnan(score) ||
               (errno == ERANGE && (std::isinf(score) || score == 0))){
                return reply.error(invalid_score, sizeof(invalid_score) - 1);
            }
            members.push_back({argument(i + 1), score});
        }
        const auto result = Database.zadd(argument(1), members);
        if(result.status == RequestStatus::WrongType) return reply.error(wrong_type, sizeof(wrong_type) - 1);
        if(result.status == RequestStatus::InvalidScore) return reply.error(invalid_score, sizeof(invalid_score) - 1);
        return reply.integer(static_cast<std::int64_t>(result.value));
    }
    if(matches("ZREM")){
        if(arguments.size() < 3) return reply.error(arity_error, sizeof(arity_error) - 1);
        std::vector<std::string_view> members;
        members.reserve(arguments.size() - 2);
        for(std::size_t i = 2; i < arguments.size(); ++i) members.push_back(argument(i));
        const auto result = Database.zrem(argument(1), members);
        if(result.status == RequestStatus::WrongType) return reply.error(wrong_type, sizeof(wrong_type) - 1);
        return reply.integer(static_cast<std::int64_t>(result.value));
    }
    if(matches("ZSCORE")){
        if(arguments.size() != 3) return reply.error(arity_error, sizeof(arity_error) - 1);
        const auto result = Database.zscore(argument(1), argument(2));
        if(result.status == RequestStatus::WrongType) return reply.error(wrong_type, sizeof(wrong_type) - 1);
        if(result.status == RequestStatus::Missing) return reply.null_bulk();
        return write_score(result.value);
    }
    if(matches("ZRANGE")){
        if(arguments.size() < 4) return reply.error(arity_error, sizeof(arity_error) - 1);
        if(arguments.size() > 5 || (arguments.size() == 5 && !matches("WITHSCORES", 4))){
            return reply.error("ERR syntax error", 16);
        }
        const auto parse_index = [](std::string_view text, std::int64_t& value) {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if(result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
            char buffer[21];
            const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return text == std::string_view(buffer, static_cast<std::size_t>(formatted.ptr - buffer));
        };
        std::int64_t start, end;
        if(!parse_index(argument(2), start) || !parse_index(argument(3), end)){
            constexpr char error[] = "ERR value is not an integer or out of range";
            return reply.error(error, sizeof(error) - 1);
        }
        const auto result = Database.zrange(argument(1), start, end);
        if(result.status == RequestStatus::WrongType) return reply.error(wrong_type, sizeof(wrong_type) - 1);
        const bool with_scores = arguments.size() == 5;
        if(!reply.array_header(result.value.size() * (with_scores ? 2 : 1))) return false;
        for(const auto& member : result.value){
            if(!reply.bulk_str(member.member.data(), member.member.size())) return false;
            if(with_scores && !write_score(member.score)) return false;
        }
        return true;
    }
    if(matches("ZRANK")){
        if(arguments.size() != 3) return reply.error(arity_error, sizeof(arity_error) - 1);
        const auto result = Database.zrank(argument(1), argument(2));
        if(result.status == RequestStatus::WrongType) return reply.error(wrong_type, sizeof(wrong_type) - 1);
        if(result.status == RequestStatus::Missing) return reply.null_bulk();
        return reply.integer(static_cast<std::int64_t>(result.value));
    }
    return reply.error("ERR unknown command", 19);
}
