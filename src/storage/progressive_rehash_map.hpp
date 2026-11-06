#pragma once

#include "../config.hpp"

#include "hash_table.hpp"
#include <cstddef>
#include <cstdlib>
#include <string>



class ProgressiveRehashMap{
private:
    HashTable current_map;
    HashTable target_map;

    bool rehashing;
    std::size_t cursor;

    std::size_t num_elements;


    void inline chechout_rehash(){
        current_map.migrate(target_map, cursor, DEFAULT_BUDGET_NODE, DEFAULT_BUDGET_BUCKET);

        if(cursor == current_map.size()){
            rehashing = false;
            current_map = target_map;
        }
    }

public:
    ProgressiveRehashMap(): current_map(INITIAL_NUM_BUCKETS), target_map(), rehashing(false), num_elements(0) {}


    const std::string* get(const std::string& key){
        const auto match = [&key](Node* node) -> bool{
            const HashNode* entry = reinterpret_cast<HashNode*>(node);
            return entry->key == key;
        };

        if(!rehashing){
            Node* search = current_map.find(DJB2(key), match);

            if(search == nullptr){
                return nullptr;
            }else{
                return &reinterpret_cast<HashNode*>(search)->value;
            }
        }else{
            const size_t cur_hash = DJB2(key);
            Node* search = current_map.find(cur_hash, match);
            if(search == nullptr) search = target_map.find(cur_hash, match);

            chechout_rehash();

            if(search == nullptr){
                return nullptr;
            }else{
                return &reinterpret_cast<HashNode*>(search)->value;
            }
        }
    }

    void set(const std::string& key, const std::string& val){
        const auto match = [&key](Node* node) -> bool{
            const HashNode* entry = reinterpret_cast<HashNode*>(node);
            return entry->key == key;
        };

        if(!rehashing){
            Node* search = current_map.find(DJB2(key), match);

            if(search == nullptr){
                HashNode* temp = new HashNode(key, val);
                current_map.insert(&temp->node);
                ++num_elements;

                if(static_cast<double>(num_elements) / current_map.size() > EXPAND_BOUND){
                    target_map = HashTable(current_map.size() << 1);
                    rehashing = true;
                    cursor = 0;
                }
            }else{
                reinterpret_cast<HashNode*>(search)->value = val;
            }
        }else{
            const std::size_t cur_hash = DJB2(key);

            Node* search = current_map.find(cur_hash, match);
            if(search == nullptr) search = target_map.find(cur_hash, match);

            if(search != nullptr){
                reinterpret_cast<HashNode*>(search)->value = val;
            }else{
                HashNode* temp = new HashNode(key, val);

                target_map.insert(&temp->node);
                ++num_elements;
            }

            chechout_rehash();
        }
    }

    bool del(const std::string& key){
        const auto match = [&key](Node* node) -> bool{
            const HashNode* entry = reinterpret_cast<HashNode*>(node);
            return entry->key == key;
        };

        if(!rehashing){
            Node* search = current_map.extract(DJB2(key), match);

            if(search == nullptr){
                return false;
            }else{
                delete reinterpret_cast<HashNode*>(search);
                --num_elements;

                if(static_cast<double>(num_elements) / current_map.size() < SHRINK_BOUND && current_map.size() > MINIMAL_NUM_BUCKETS){
                    target_map = HashTable(current_map.size() >> 1);
                    rehashing = true;
                    cursor = 0;
                }

                return true;
            }
        }else{
            std::size_t cur_hash = DJB2(key);

            Node* search = current_map.extract(cur_hash, match);
            if(search == nullptr) search = target_map.extract(cur_hash, match);

            chechout_rehash();

            if(search == nullptr){
                return false;
            }else{
                delete reinterpret_cast<HashNode*>(search);
                --num_elements;

                return true;
            }
        }
    }


    ~ProgressiveRehashMap() {
        const auto dispose = [](Node* node) noexcept {
            delete reinterpret_cast<HashNode*>(node);
        };

        current_map.clear(dispose);
        target_map.clear(dispose);
    }
};
