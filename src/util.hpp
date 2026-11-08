#pragma once

#include <chrono>
#include <cstdint>

inline std::int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now();

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
}