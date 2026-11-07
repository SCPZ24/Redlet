#pragma once

#include "../config.hpp"

#include "hash_table.hpp"
#include "node.hpp"
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <type_traits>



// Entry是整个业务单元。
// 自身的key, value是业务数据，本质为存储的对象
// 继承自父类的是索引元（可以多继承），用于插入哈希表、AVL等定位器。
template<typename Entry>
class ProgressiveRehashMap{
    static_assert(std::is_convertible_v<Entry*, HashHook*>);
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
    ProgressiveRehashMap():
        current_map(INITIAL_NUM_BUCKETS),
        target_map(),
        rehashing(false),
        num_elements(0)
        {}


    Entry* find(std::string_view key){
        const std::size_t cur_hash = std::hash<std::string_view>{}(key);
        const auto match = [&key](HashHook* node) -> bool{
            const Entry* entry = static_cast<Entry*>(node);
            return entry->key == key;
        };

        if(!rehashing){
            HashHook* search = current_map.find(cur_hash, match);

            if(search == nullptr){
                return nullptr;
            }else{
                return static_cast<Entry*>(search);
            }
        }else{
            HashHook* search = current_map.find(cur_hash, match);
            if(search == nullptr) search = target_map.find(cur_hash, match);

            chechout_rehash();

            if(search == nullptr){
                return nullptr;
            }else{
                return static_cast<Entry*>(search);
            }
        }
    }

    // The caller retains the entry when insertion fails.
    bool insert(Entry* entry){
        const std::string_view key = entry->key;
        const std::size_t cur_hash = std::hash<std::string_view>{}(key);
        const auto match = [&key](HashHook* node) -> bool{
            const Entry* entry = static_cast<Entry*>(node);
            return entry->key == key;
        };

        if(!rehashing){
            HashHook* search = current_map.find(cur_hash, match);

            if(search == nullptr){
                // Allocate before linking so failure leaves entry unlinked.
                if(static_cast<double>(num_elements + 1) / current_map.size() > EXPAND_BOUND){
                    target_map = HashTable(current_map.size() << 1);
                    rehashing = true;
                    cursor = 0;
                }
                entry->hash = cur_hash;
                current_map.insert(entry);
                ++num_elements;
            }else{
                return false;
            }
        }else{

            HashHook* search = current_map.find(cur_hash, match);
            if(search == nullptr) search = target_map.find(cur_hash, match);

            if(search != nullptr){
                chechout_rehash();
                return false;
            }else{
                entry->hash = cur_hash;
                target_map.insert(entry);
                ++num_elements;
            }

            chechout_rehash();
        }
        return true;
    }

    // Unlink without destroying the entry.
    Entry* extract(std::string_view key){
        const std::size_t cur_hash = std::hash<std::string_view>{}(key);
        const auto match = [&key](HashHook* node) -> bool{
            const Entry* entry = static_cast<Entry*>(node);
            return entry->key == key;
        };

        if(!rehashing){
            HashHook* search = current_map.extract(cur_hash, match);

            if(search == nullptr){
                return nullptr;
            }else{
                --num_elements;

                if(static_cast<double>(num_elements) / current_map.size() < SHRINK_BOUND && current_map.size() > MINIMAL_NUM_BUCKETS){
                    target_map = HashTable(current_map.size() >> 1);
                    rehashing = true;
                    cursor = 0;
                }

                return static_cast<Entry*>(search);
            }
        }else{

            HashHook* search = current_map.extract(cur_hash, match);
            if(search == nullptr) search = target_map.extract(cur_hash, match);

            chechout_rehash();

            if(search == nullptr){
                return nullptr;
            }else{
                --num_elements;

                return static_cast<Entry*>(search);
            }
        }
    }


    template<typename Dispose>
    void clear(const Dispose& dispose) noexcept {
        static_assert(std::is_nothrow_invocable_v<const Dispose&, Entry*>);
        const auto release = [&dispose](HashHook* node) noexcept {
            dispose(static_cast<Entry*>(node));
        };
        current_map.clear(release);
        target_map.clear(release);
        num_elements = 0;
        rehashing = false;
    }
};
