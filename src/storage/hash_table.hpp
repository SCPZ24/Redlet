#pragma once

#include "node.cpp"
#include <cstddef>
#include <cstdlib>
#include <system_error>
#include <utility>



class HashTable{
private:
    Node** buckets = nullptr;

    std::size_t num_buckets = 0;
public:
    HashTable(size_t _size): num_buckets(_size){
        buckets = static_cast<Node**>(std::calloc(_size, sizeof(Node*)));

        if(buckets == nullptr){
            throw std::system_error(errno, std::system_category(), "calloc");
        }
    }

    template<typename Match>
    Node* find(std::size_t hash, Match match){
        const std::size_t bucket = hash & (num_buckets - 1);
        Node* current_node = buckets[bucket];

        while(current_node != nullptr){
            if(current_node->hash == hash && match(current_node)){
                return current_node;
            }
            
            current_node = current_node->next;
        }

        return nullptr;
    }

    void inline insert(Node* node){
        const std::size_t bucket = node->hash & (num_buckets - 1);
        
        node->next = buckets[bucket];
        buckets[bucket] = node;
    }

    template<typename Match>
    Node* extract(std::size_t hash, Match match){
        const std::size_t bucket = hash & (num_buckets - 1);
        Node** ptr_current_node = &buckets[bucket];

        while(*ptr_current_node != nullptr){
            if((*ptr_current_node)->hash == hash && match(*ptr_current_node)){
                Node* temp = *ptr_current_node;
                *ptr_current_node = temp->next;
                temp->next = nullptr;
                return temp;
            }

            ptr_current_node = &(*ptr_current_node)->next;
        }

        return nullptr;
    }

    void migrate(HashTable& dst, std::size_t& current_bucket, std::size_t budget_node, std::size_t budget_bucket){
        // normal: 0 <= current_bucket < num_buckets
        // finish: current_bucket == num_buckets

        while(budget_node > 0 && budget_bucket > 0 && current_bucket < num_buckets){
            if(buckets[current_bucket] == nullptr){
                ++current_bucket;
                --budget_bucket;
                continue;
            }

            Node* temp = buckets[current_bucket];
            buckets[current_bucket] = temp->next;
            dst.insert(temp);
            --budget_node;
        }
    }


    std::size_t size() const{
        return num_buckets;
    }

    // Intrusive nodes belong to their enclosing objects; only free the bucket array.
    ~HashTable(){
        std::free(buckets);
    }

    // Will even free the src. make sure all the nodes are transferred to new Table correctly.
    HashTable& operator=(HashTable& src){
        if (this == &src) return *this;
        std::free(buckets);
        buckets = std::exchange(src.buckets, nullptr);
        num_buckets = std::exchange(src.num_buckets, 0);
        return *this;
    }

    HashTable(const HashTable& src) = delete;
    HashTable& operator=(const HashTable& src) = delete;

};
