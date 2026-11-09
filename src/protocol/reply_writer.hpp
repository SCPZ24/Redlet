#pragma once

#include "../config.hpp"
#include "../buffer/output_buffer.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>



class ReplyWriter{
private:
    OutputBuffer& output;

    bool can_append(std::size_t payload, std::size_t framing) const {
        if(output.pending_bytes() > MAX_OUTPUT_BYTES) return false;
        const std::size_t room = MAX_OUTPUT_BYTES - output.pending_bytes();
        return framing <= room && payload <= room - framing;
    }

public:
    ReplyWriter(OutputBuffer& outcoming): output(outcoming){}

    bool inline raw_str(const char* data, std::size_t size) const{
        if(!can_append(size, 3)) return false;

        return output.push_back('+')
            && output.append(std::string_view(data, size))
            && output.append("\r\n");
    }

    bool inline integer(std::int64_t value) const{
        // Enough for all decimal digits and the sign, including INT64_MIN.
        char digits[std::numeric_limits<std::int64_t>::digits10 + 2];
        const auto result = std::to_chars(digits, digits + sizeof(digits), value);
        const auto size = static_cast<std::size_t>(result.ptr - digits);
        if(!can_append(size, 3)) return false;
        return output.push_back(':')
            && output.append(std::string_view(digits, size))
            && output.append("\r\n");
    }

    bool inline error(const char* data, std::size_t size) const{
        if(!can_append(size, 3)) return false;
        return output.push_back('-')
            && output.append(std::string_view(data, size))
            && output.append("\r\n");
    }

    bool inline bulk_str(const char* data, std::size_t size) const{
        char digits[std::numeric_limits<std::size_t>::digits10 + 1];
        const auto result = std::to_chars(digits, digits + sizeof(digits), size);
        const auto length_size = static_cast<std::size_t>(result.ptr - digits);
        if(!can_append(size, length_size + 5)) return false;
        return output.push_back('$')
            && output.append(std::string_view(digits, length_size))
            && output.append("\r\n")
            && output.append(std::string_view(data, size))
            && output.append("\r\n");
    }

    bool inline array_header(std::size_t count) const{
        char digits[std::numeric_limits<std::size_t>::digits10 + 1];
        const auto result = std::to_chars(digits, digits + sizeof(digits), count);
        const auto size = static_cast<std::size_t>(result.ptr - digits);
        if(!can_append(size, 3)) return false;
        return output.push_back('*')
            && output.append(std::string_view(digits, size))
            && output.append("\r\n");
    }

    bool inline null_bulk() const{
        if(!can_append(0, 5)) return false;
        return output.append("$-1\r\n");
    }

    ReplyWriter(const ReplyWriter&) = delete;
    ReplyWriter& operator=(const ReplyWriter&) = delete;
};
