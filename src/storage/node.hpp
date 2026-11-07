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





struct AVLHook{
    AVLHook* parent;

    AVLHook* left;
    AVLHook* right;

    size_t count;

    size_t height;


    AVLHook(AVLHook* _parent = nullptr, AVLHook* _left = nullptr, AVLHook* _right = nullptr, size_t _count = 0, size_t _height = 0):
        parent(_parent), left(_left), right(_right), count(_count), height(_height) {}
};





class AVLNode: AVLHook, HashHook{
private:
    AVLHook avl_node;
    HashHook hash_node;

    std::string key;

    double score;

public:

};