#pragma once

#include <string>
#include <string_view>
#include <variant>

#include "progressive_rehash_map.hpp"
#include "sorted_set.hpp"

using Entry = HashNode<std::variant<std::string, SortedSet*>>;

enum class RequestStatus{
    OK,
    Missing,
    WrongType,
    InvalidScore
};

struct GetResult{
    RequestStatus status;
    const std::string* value;
};


class Database{
private:
    ProgressiveRehashMap<Entry> data;

public:
    Database(): data() {}

    GetResult get(std::string_view key){
        const Entry* entry = data.find(key);
        if(entry == nullptr) return {RequestStatus::Missing, nullptr};

        const auto* value = std::get_if<std::string>(&entry->value);
        if(value == nullptr) return {RequestStatus::WrongType, nullptr};
        return {RequestStatus::OK, value};
    }

    void set(std::string_view key, std::string_view value){
        if(Entry* entry = data.find(key)){
            if(auto* text = std::get_if<std::string>(&entry->value)){
                *text = value;
            }else{
                SortedSet* sorted_set = std::get<SortedSet*>(entry->value);
                entry->value = std::string(value);
                delete sorted_set;
            }
        }else{
            data.insert(key, std::string(value));
        }
    }

    bool del(std::string_view key){
        Entry* entry = data.extract(key);
        if(entry == nullptr) return false;

        if(auto* sorted_set = std::get_if<SortedSet*>(&entry->value)){
            delete *sorted_set;
        }
        delete entry;
        return true;
    }

    // TODO: 补全ZADD ZREM ZSCORE ZRANGE ZRANK接口


    ~Database(){
        data.clear([](Entry* entry) noexcept {
            if(auto* sorted_set = std::get_if<SortedSet*>(&entry->value)){
                delete *sorted_set;
            }
            delete entry;
        });
    }
};



inline Database Database;
