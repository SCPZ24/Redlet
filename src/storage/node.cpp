#pragma once


#include <cstddef>
#include <string>



size_t DJB2(const std::string& str){
    size_t hash = 5381;
    for(unsigned char byte: str){
        hash = (hash << 5) + hash + byte;
    }
    return hash;
}



struct Node{
    size_t hash;
    Node* next;

    Node(const std::string& str, Node* _next): hash(DJB2(str)), next(_next){}
};






struct HashNode{
    Node node;
    std::string key;
    std::string value;
};