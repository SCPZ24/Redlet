#pragma once

#include "node.hpp"
#include <cstddef>
#include <cstdlib>
#include <system_error>
#include <utility>



class HashTable{
private:
    HashHook** buckets = nullptr;

    std::size_t num_buckets = 0;
public:
    HashTable() = default;

    HashTable(size_t _size): num_buckets(_size){
        buckets = static_cast<HashHook**>(std::calloc(_size, sizeof(HashHook*)));

        if(buckets == nullptr){
            throw std::system_error(errno, std::system_category(), "calloc");
        }
    }

    template<typename Match>
    HashHook* find(std::size_t hash, const Match& match){
        const std::size_t bucket = hash & (num_buckets - 1);
        HashHook* current_node = buckets[bucket];

        while(current_node != nullptr){
            if(current_node->hash == hash && match(current_node)){
                return current_node;
            }
            
            current_node = current_node->next;
        }

        return nullptr;
    }

    void inline insert(HashHook* node){
        const std::size_t bucket = node->hash & (num_buckets - 1);
        
        node->next = buckets[bucket];
        buckets[bucket] = node;
    }

    template<typename Match>
    HashHook* extract(std::size_t hash, const Match& match){
        const std::size_t bucket = hash & (num_buckets - 1);
        HashHook** ptr_current_node = &buckets[bucket];

        while(*ptr_current_node != nullptr){
            if((*ptr_current_node)->hash == hash && match(*ptr_current_node)){
                HashHook* temp = *ptr_current_node;
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

            HashHook* temp = buckets[current_bucket];
            buckets[current_bucket] = temp->next;
            dst.insert(temp);
            --budget_node;
        }
    }


    std::size_t size() const{
        return num_buckets;
    }

    template<typename Dispose>
    void clear(const Dispose& dispose) noexcept {
        for (std::size_t bucket = 0; bucket < num_buckets; ++bucket) {
            while (buckets[bucket] != nullptr) {
                HashHook* node = buckets[bucket];
                buckets[bucket] = node->next;
                node->next = nullptr;
                dispose(node);
            }
        }
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

    HashTable& operator=(HashTable&& src){
        return operator=(src);
    }

    HashTable(const HashTable& src) = delete;
    HashTable& operator=(const HashTable& src) = delete;

};
