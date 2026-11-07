#pragma once


#include <cstddef>
#include <string>
#include <string_view>
#include <utility>



struct HashHook{
    size_t hash;
    HashHook* next;

    HashHook(size_t _hash, HashHook* _next): hash(_hash), next(_next){}
};






template<typename Value>
struct HashNode : HashHook{
    std::string key;
    Value value;

    template<typename T>
    HashNode(std::string_view _key, T&& _value): HashHook(0, nullptr), key(_key), value(std::forward<T>(_value)){}
};
