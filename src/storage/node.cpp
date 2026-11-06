#pragma once


#include <cstddef>
#include <string>



struct Node{
    size_t hash;
    Node* next;

    Node(size_t _hash, Node* _next): hash(_hash), next(_next){}
};






struct HashNode{
    Node node;
    std::string key;
    std::string value;

    HashNode(const std::string& _key, const std::string& _value, size_t _hash): node(_hash, nullptr), key(_key), value(_value){}
};