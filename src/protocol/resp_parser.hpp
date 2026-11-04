#pragma once

#include "../buffer/byte_range.hpp"
#include "../config.hpp"

#include <cstddef>
#include <vector>

enum class ParseStatus { NeedMore, Complete, Invalid };

struct ArgumentRange {
    std::size_t offset;
    std::size_t length;
};

// Each call supplies the same request prefix, optionally extended at its tail.
// Only offsets survive calls, so reallocating the input storage is safe.
class RespParser {
public:
    ParseStatus parse(ByteRange input) {
        if (status_ != ParseStatus::NeedMore) return status_;

        while (true) {
            if (stage_ == Stage::BulkData) {
                const std::size_t end = position_ + number_;
                if (input.size() <= end) return status_;
                if (input[end] != '\r') return invalidate();
                if (input.size() <= end + 1) return status_;
                if (input[end + 1] != '\n') return invalidate();
                arguments_.push_back({position_, number_});
                position_ = end + 2;
                if (--remaining_ == 0) {
                    status_ = ParseStatus::Complete;
                    return status_;
                }
                stage_ = Stage::BulkPrefix;
                continue;
            }

            if (position_ >= MAX_REQUEST_BYTES) return invalidate();
            if (position_ >= input.size()) return status_;
            const char byte = input[position_++];

            switch (stage_) {
            case Stage::ArrayPrefix:
                if (byte != '*') return invalidate();
                begin_number(Stage::ArrayDigits);
                break;
            case Stage::BulkPrefix:
                if (byte != '$') return invalidate();
                begin_number(Stage::BulkDigits);
                break;
            case Stage::ArrayDigits:
            case Stage::BulkDigits: {
                if (byte == '\r') {
                    if (!has_digit_) return invalidate();
                    if (stage_ == Stage::ArrayDigits) {
                        if (number_ == 0) return invalidate();
                        remaining_ = number_;
                        stage_ = Stage::ArrayLF;
                    } else {
                        stage_ = Stage::BulkLF;
                    }
                    break;
                }
                if (byte < '0' || byte > '9') return invalidate();
                const std::size_t digit = static_cast<std::size_t>(byte - '0');
                const std::size_t limit = stage_ == Stage::ArrayDigits
                    ? MAX_REQUEST_ARGS : MAX_BULK_BYTES;
                if (number_ > limit / 10 ||
                    (number_ == limit / 10 && digit > limit % 10)) {
                    return invalidate();
                }
                number_ = number_ * 10 + digit;
                has_digit_ = true;
                break;
            }
            case Stage::ArrayLF:
                if (byte != '\n') return invalidate();
                stage_ = Stage::BulkPrefix;
                break;
            case Stage::BulkLF:
                if (byte != '\n') return invalidate();
                // Check subtraction before computing the payload end.
                if (MAX_REQUEST_BYTES - position_ < 2 ||
                    number_ > MAX_REQUEST_BYTES - position_ - 2) {
                    return invalidate();
                }
                stage_ = Stage::BulkData;
                break;
            case Stage::BulkData:
                break; // Handled above without scanning the payload.
            }
        }
    }

    ParseStatus status() const noexcept { return status_; }
    const std::vector<ArgumentRange>& arguments() const noexcept { return arguments_; }
    // Meaningful only after Complete; includes all RESP framing bytes.
    std::size_t request_size() const noexcept { return position_; }

    void reset() {
        stage_ = Stage::ArrayPrefix;
        status_ = ParseStatus::NeedMore;
        position_ = 0;
        number_ = 0;
        remaining_ = 0;
        has_digit_ = false;
        arguments_.clear();
    }

private:
    enum class Stage { ArrayPrefix, ArrayDigits, ArrayLF, BulkPrefix, BulkDigits, BulkLF, BulkData };
    Stage stage_ = Stage::ArrayPrefix;
    ParseStatus status_ = ParseStatus::NeedMore;
    std::size_t position_ = 0;
    std::size_t number_ = 0;
    std::size_t remaining_ = 0;
    bool has_digit_ = false;
    std::vector<ArgumentRange> arguments_;

    void begin_number(Stage stage) {
        stage_ = stage;
        number_ = 0;
        has_digit_ = false;
    }

    ParseStatus invalidate() {
        status_ = ParseStatus::Invalid;
        return status_;
    }
};
