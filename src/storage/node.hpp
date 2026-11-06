#pragma once


#include <cstddef>
#include <string>
#include <string_view>



struct Node{
    size_t hash;
    Node* next;

    Node(size_t _hash, Node* _next): hash(_hash), next(_next){}
};






struct HashNode{
    Node node;
    std::string key;
    std::string value;

    HashNode(std::string_view _key, std::string_view _value, size_t _hash): node(_hash, nullptr), key(_key), value(_value){}
};
