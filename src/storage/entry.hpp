#pragma once

#include "node.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

class SortedSet;


inline constexpr std::size_t NO_HEAP_INDEX = std::numeric_limits<std::size_t>::max();


struct Entry : HashNode<std::variant<std::string, SortedSet*>>{
    std::size_t heap_idx = NO_HEAP_INDEX;

    // 初始化列表调用基类构造函数，由基类初始化 key 和 value。
    Entry(std::string_view key, std::variant<std::string, SortedSet*> value)
        : HashNode<std::variant<std::string, SortedSet*>>(key, std::move(value)) {}
};
