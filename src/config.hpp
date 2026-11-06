#pragma once

#include <cstddef>

inline constexpr char arity_error[] = "ERR wrong number of arguments";

inline constexpr size_t MAX_BULK_BYTES = 1024 * 1024;
inline constexpr size_t MAX_REQUEST_BYTES = 2 * 1024 * 1024;
inline constexpr size_t MAX_REQUEST_ARGS = 1024;
inline constexpr size_t MAX_OUTPUT_BYTES = 4 * 1024 * 1024;

inline constexpr unsigned short LISTENTING_PORT = 6379;

inline constexpr int MAX_EPOLL_EVENTS = 1024;

inline constexpr int INPUT_BUFFER_SIZE = 1024;
inline constexpr int OUTPUT_BUFFER_SIZE = 1024;


inline constexpr double EXPANSION_BOUND = 1;
inline constexpr double SHRINL_BOUND = 0.25;