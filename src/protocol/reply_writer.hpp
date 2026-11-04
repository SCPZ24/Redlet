#pragma once

#include "../config.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>



class ReplyWriter{
private:
    std::string& output;

    bool can_append(std::size_t payload, std::size_t framing) const {
        if(output.size() > MAX_OUTPUT_BYTES) return false;
        const std::size_t room = MAX_OUTPUT_BYTES - output.size();
        return payload + framing <= room;
    }

public:
    ReplyWriter(std::string& outcoming): output(outcoming){}

    bool inline raw_str(const char* data, std::size_t size){
        if(!can_append(size, 3)) return false;

        output.push_back('+');
        output.append(data, size);
        output.append("\r\n");
        return true;
    }

    bool inline integer(std::int64_t value){
        // Enough for all decimal digits and the sign, including INT64_MIN.
        char digits[std::numeric_limits<std::int64_t>::digits10 + 2];
        const auto result = std::to_chars(digits, digits + sizeof(digits), value);
        const auto size = static_cast<std::size_t>(result.ptr - digits);
        if(!can_append(size, 3)) return false;
        output.push_back(':');
        output.append(digits, size);
        output.append("\r\n");
        return true;
    }

    bool inline error(const char* data, std::size_t size){
        if(!can_append(size, 3)) return false;
        output.push_back('-');
        output.append(data, size);
        output.append("\r\n");
        return true;
    }

    bool inline bulk_str(const char* data, std::size_t size){
        char digits[std::numeric_limits<std::size_t>::digits10 + 1];
        const auto result = std::to_chars(digits, digits + sizeof(digits), size);
        const auto length_size = static_cast<std::size_t>(result.ptr - digits);
        if(!can_append(size, length_size + 5)) return false;
        output.push_back('$');
        output.append(digits, length_size);
        output.append("\r\n");
        output.append(data, size);
        output.append("\r\n");
        return true;
    }

    bool inline null_bulk(){
        if(!can_append(0, 5)) return false;
        output.append("$-1\r\n");
        return true;
    }


    ReplyWriter(const ReplyWriter&) = delete;
    ReplyWriter& operator=(const ReplyWriter&) = delete;
};
