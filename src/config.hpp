#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr char arity_error[] = "ERR wrong number of arguments";

inline constexpr size_t MAX_BULK_BYTES = 1024 * 1024;
inline constexpr size_t MAX_REQUEST_BYTES = 2 * 1024 * 1024;
inline constexpr size_t MAX_REQUEST_ARGS = 1024;
inline constexpr size_t MAX_OUTPUT_BYTES = 4 * 1024 * 1024;

inline constexpr unsigned short LISTENTING_PORT = 6379;

inline constexpr int MAX_EPOLL_EVENTS = 1024;

inline constexpr int INPUT_BUFFER_SIZE = 1024;
inline constexpr int OUTPUT_BUFFER_SIZE = 1024;

inline constexpr double EXPAND_BOUND = 1;
inline constexpr double SHRINK_BOUND = 0.25;

inline constexpr size_t INITIAL_NUM_BUCKETS = 8;
inline constexpr size_t MINIMAL_NUM_BUCKETS = 4;

inline constexpr size_t DEFAULT_BUDGET_NODE = 128;
inline constexpr size_t DEFAULT_BUDGET_BUCKET = 256;

inline constexpr size_t DEFAULT_WAKE_DURATION = 1000;

inline constexpr std::int64_t DEFAULT_CONNECTION_TIMEOUT_MS = 60000; //1分钟

inline constexpr size_t DEFAULT_EXPIRE_BUDGET = 64;

inline constexpr size_t DEFAULT_WORKER_COUNT = 16;