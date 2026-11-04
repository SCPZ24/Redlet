#pragma once

#include <cstddef>
#include <string_view>

// Borrowed bytes. The owner must keep the storage stable while this range is used.
class ByteRange {
public:
    ByteRange(const char* data, std::size_t size) : bytes(data, size) {}

    const char* data() const noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
    char operator[](std::size_t index) const { return bytes[index]; }

private:
    std::string_view bytes;
};
